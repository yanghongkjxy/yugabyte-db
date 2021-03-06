// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/util/net/socket.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <limits>
#include <string>

#include <glog/logging.h>

#include "yb/gutil/basictypes.h"
#include "yb/gutil/stringprintf.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/errno.h"
#include "yb/util/flag_tags.h"
#include "yb/util/monotime.h"
#include "yb/util/net/net_util.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/random.h"
#include "yb/util/random_util.h"
#include "yb/util/subprocess.h"

DEFINE_string(local_ip_for_outbound_sockets, "",
              "IP to bind to when making outgoing socket connections. "
              "This must be an IP address of the form A.B.C.D, not a hostname. "
              "Advanced parameter, subject to change.");
TAG_FLAG(local_ip_for_outbound_sockets, experimental);

DEFINE_bool(socket_inject_short_recvs, false,
            "Inject short recv() responses which return less data than "
            "requested");
TAG_FLAG(socket_inject_short_recvs, hidden);
TAG_FLAG(socket_inject_short_recvs, unsafe);

namespace yb {

Socket::Socket()
  : fd_(-1) {
}

Socket::Socket(int fd)
  : fd_(fd) {
}

void Socket::Reset(int fd) {
  ignore_result(Close());
  fd_ = fd;
}

int Socket::Release() {
  int fd = fd_;
  fd_ = -1;
  return fd;
}

Socket::~Socket() {
  auto status = Close();
  if (!status.ok()) {
    LOG(WARNING) << "Failed to close socket: " << status.ToString();
  }
}

Status Socket::Close() {
  if (fd_ < 0)
    return Status::OK();
  int err, fd = fd_;
  fd_ = -1;
  if (::close(fd) < 0) {
    err = errno;
    return STATUS(NetworkError,
                  strings::Substitute("Close error: $0", ErrnoToString(err)),
                  Slice(),
                  err);
  }
  return Status::OK();
}

Status Socket::Shutdown(bool shut_read, bool shut_write) {
  DCHECK_GE(fd_, 0);
  int flags = 0;
  if (shut_read && shut_write) {
    flags |= SHUT_RDWR;
  } else if (shut_read) {
    flags |= SHUT_RD;
  } else if (shut_write) {
    flags |= SHUT_WR;
  }
  if (::shutdown(fd_, flags) < 0) {
    int err = errno;
    return STATUS(NetworkError, std::string("shutdown error: ") +
                                ErrnoToString(err), Slice(), err);
  }
  return Status::OK();
}

int Socket::GetFd() const {
  return fd_;
}

bool Socket::IsTemporarySocketError(const Status& status) {
  if (!status.IsNetworkError()) {
    return false;
  }
  auto err = status.error_code();
  return err == EAGAIN || err == EWOULDBLOCK || err == EINTR || err == EINPROGRESS;
}

#if defined(__linux__)

Status Socket::Init(int flags) {
  auto family = flags & FLAG_IPV6 ? AF_INET6 : AF_INET;
  int nonblocking_flag = (flags & FLAG_NONBLOCKING) ? SOCK_NONBLOCK : 0;
  Reset(::socket(family, SOCK_STREAM | SOCK_CLOEXEC | nonblocking_flag, 0));
  if (fd_ < 0) {
    int err = errno;
    return STATUS(NetworkError, std::string("error opening socket: ") +
                                ErrnoToString(err), Slice(), err);
  }

  return Status::OK();
}

#else

Status Socket::Init(int flags) {
  Reset(::socket(flags & FLAG_IPV6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0));
  if (fd_ < 0) {
    int err = errno;
    return STATUS(NetworkError, std::string("error opening socket: ") +
                                ErrnoToString(err), Slice(), err);
  }
  RETURN_NOT_OK(SetNonBlocking(flags & FLAG_NONBLOCKING));
  RETURN_NOT_OK(SetCloseOnExec());

  // Disable SIGPIPE.
  int set = 1;
  if (setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set)) == -1) {
    int err = errno;
    return STATUS(NetworkError, std::string("failed to set SO_NOSIGPIPE: ") +
                                ErrnoToString(err), Slice(), err);
  }

  return Status::OK();
}

#endif // defined(__linux__)

Status Socket::SetNoDelay(bool enabled) {
  int flag = enabled ? 1 : 0;
  if (setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == -1) {
    int err = errno;
    return STATUS(NetworkError, std::string("failed to set TCP_NODELAY: ") +
                                ErrnoToString(err), Slice(), err);
  }
  return Status::OK();
}

Status Socket::SetNonBlocking(bool enabled) {
  int curflags = ::fcntl(fd_, F_GETFL, 0);
  if (curflags == -1) {
    int err = errno;
    return STATUS(NetworkError,
        StringPrintf("Failed to get file status flags on fd %d", fd_),
        ErrnoToString(err), err);
  }
  int newflags = (enabled) ? (curflags | O_NONBLOCK) : (curflags & ~O_NONBLOCK);
  if (::fcntl(fd_, F_SETFL, newflags) == -1) {
    int err = errno;
    if (enabled) {
      return STATUS(NetworkError,
          StringPrintf("Failed to set O_NONBLOCK on fd %d", fd_),
          ErrnoToString(err), err);
    } else {
      return STATUS(NetworkError,
          StringPrintf("Failed to clear O_NONBLOCK on fd %d", fd_),
          ErrnoToString(err), err);
    }
  }
  return Status::OK();
}

Status Socket::IsNonBlocking(bool* is_nonblock) const {
  int curflags = ::fcntl(fd_, F_GETFL, 0);
  if (curflags == -1) {
    int err = errno;
    return STATUS(NetworkError,
        StringPrintf("Failed to get file status flags on fd %d", fd_),
        ErrnoToString(err), err);
  }
  *is_nonblock = ((curflags & O_NONBLOCK) != 0);
  return Status::OK();
}

Status Socket::SetCloseOnExec() {
  int curflags = fcntl(fd_, F_GETFD, 0);
  if (curflags == -1) {
    int err = errno;
    Reset(-1);
    return STATUS(NetworkError, std::string("fcntl(F_GETFD) error: ") +
                                ErrnoToString(err), Slice(), err);
  }
  if (fcntl(fd_, F_SETFD, curflags | FD_CLOEXEC) == -1) {
    int err = errno;
    Reset(-1);
    return STATUS(NetworkError, std::string("fcntl(F_SETFD) error: ") +
                                ErrnoToString(err), Slice(), err);
  }
  return Status::OK();
}

Status Socket::SetSendTimeout(const MonoDelta& timeout) {
  return SetTimeout(SO_SNDTIMEO, "SO_SNDTIMEO", timeout);
}

Status Socket::SetRecvTimeout(const MonoDelta& timeout) {
  return SetTimeout(SO_RCVTIMEO, "SO_RCVTIMEO", timeout);
}

Status Socket::SetReuseAddr(bool flag) {
  int err;
  int int_flag = flag ? 1 : 0;
  if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &int_flag, sizeof(int_flag)) == -1) {
    err = errno;
    return STATUS(NetworkError, std::string("failed to set SO_REUSEADDR: ") +
                                ErrnoToString(err), Slice(), err);
  }
  return Status::OK();
}

Status Socket::BindAndListen(const Endpoint& sockaddr,
                             int listenQueueSize) {
  RETURN_NOT_OK(SetReuseAddr(true));
  RETURN_NOT_OK(Bind(sockaddr));
  RETURN_NOT_OK(Listen(listenQueueSize));
  return Status::OK();
}

Status Socket::Listen(int listen_queue_size) {
  if (listen(fd_, listen_queue_size)) {
    int err = errno;
    return STATUS(NetworkError, "listen() error", ErrnoToString(err));
  }
  return Status::OK();
}

namespace {

enum class EndpointType {
  REMOTE,
  LOCAL,
};

Status GetEndpoint(EndpointType type, int fd, Endpoint* out) {
  Endpoint temp;
  DCHECK_GE(fd, 0);
  socklen_t len = temp.capacity();
  auto result = type == EndpointType::LOCAL ? getsockname(fd, temp.data(), &len)
                                            : getpeername(fd, temp.data(), &len);
  if (result == -1) {
    int err = errno;
    const std::string prefix = type == EndpointType::LOCAL ? "getsockname" : "getpeername";
    return STATUS(NetworkError,
                  prefix + " error: " + ErrnoToString(err),
                  Slice(),
                  err);
  }
  temp.resize(len);
  *out = temp;
  return Status::OK();
}

} // namespace

Status Socket::GetSocketAddress(Endpoint* out) const {
  return GetEndpoint(EndpointType::LOCAL, fd_, out);
}

Status Socket::GetPeerAddress(Endpoint* out) const {
  return GetEndpoint(EndpointType::REMOTE, fd_, out);
}

Status Socket::Bind(const Endpoint& endpoint, bool explain_addr_in_use) {
  DCHECK_GE(fd_, 0);
  if (PREDICT_FALSE(::bind(fd_, endpoint.data(), endpoint.size()))) {
    int err = errno;
    Status s = STATUS(NetworkError,
                      strings::Substitute("Error binding socket to $0: $1",
                                          ToString(endpoint),
                                          ErrnoToString(err)),
                      Slice(),
                      err);

    if (s.IsNetworkError() && s.error_code() == EADDRINUSE && explain_addr_in_use &&
        endpoint.port() != 0) {
      TryRunLsof(endpoint);
    }
    return s;
  }

  return Status::OK();
}

Status Socket::Accept(Socket *new_conn, Endpoint* remote, int flags) {
  TRACE_EVENT0("net", "Socket::Accept");
  Endpoint temp;
  socklen_t olen = temp.capacity();
  DCHECK_GE(fd_, 0);
#if defined(__linux__)
  int accept_flags = SOCK_CLOEXEC;
  if (flags & FLAG_NONBLOCKING) {
    accept_flags |= SOCK_NONBLOCK;
  }
  new_conn->Reset(::accept4(fd_, temp.data(), &olen, accept_flags));
  if (new_conn->GetFd() < 0) {
    int err = errno;
    return STATUS(NetworkError, std::string("accept4(2) error: ") +
                                ErrnoToString(err), Slice(), err);
  }
#else
  new_conn->Reset(::accept(fd_, temp.data(), &olen));
  if (new_conn->GetFd() < 0) {
    int err = errno;
    return STATUS(NetworkError, std::string("accept(2) error: ") +
                                ErrnoToString(err), Slice(), err);
  }
  RETURN_NOT_OK(new_conn->SetNonBlocking(flags & FLAG_NONBLOCKING));
  RETURN_NOT_OK(new_conn->SetCloseOnExec());
#endif // defined(__linux__)
  temp.resize(olen);

  *remote = temp;
  TRACE_EVENT_INSTANT1("net", "Accepted", TRACE_EVENT_SCOPE_THREAD,
                       "remote", ToString(*remote));
  return Status::OK();
}

Status Socket::BindForOutgoingConnection() {
  boost::system::error_code ec;
  auto bind_address = IpAddress::from_string(FLAGS_local_ip_for_outbound_sockets, ec);
  CHECK(!ec)
    << "Invalid local IP set for 'local_ip_for_outbound_sockets': '"
    << FLAGS_local_ip_for_outbound_sockets << "': " << ec;

  RETURN_NOT_OK(Bind(Endpoint(bind_address, 0)));
  return Status::OK();
}

Status Socket::Connect(const Endpoint& remote) {
  TRACE_EVENT1("net", "Socket::Connect", "remote", ToString(remote));

  if (PREDICT_FALSE(!FLAGS_local_ip_for_outbound_sockets.empty())) {
    RETURN_NOT_OK(BindForOutgoingConnection());
  }

  DCHECK_GE(fd_, 0);
  if (::connect(fd_, remote.data(), remote.size()) < 0) {
    int err = errno;
    return STATUS(NetworkError, std::string("connect(2) error: ") +
                                ErrnoToString(err), Slice(), err);
  }
  return Status::OK();
}

Status Socket::GetSockError() const {
  int val = 0, ret;
  socklen_t val_len = sizeof(val);
  DCHECK_GE(fd_, 0);
  ret = ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &val, &val_len);
  if (ret) {
    int err = errno;
    return STATUS(NetworkError, std::string("getsockopt(SO_ERROR) failed: ") +
                                ErrnoToString(err), Slice(), err);
  }
  if (val != 0) {
    return STATUS(NetworkError, ErrnoToString(val), Slice(), val);
  }
  return Status::OK();
}

Status Socket::Write(const uint8_t *buf, int32_t amt, int32_t *nwritten) {
  if (amt <= 0) {
    return STATUS(NetworkError,
              StringPrintf("invalid send of %" PRId32 " bytes",
                           amt), Slice(), EINVAL);
  }
  DCHECK_GE(fd_, 0);
  int res = ::send(fd_, buf, amt, MSG_NOSIGNAL);
  if (res < 0) {
    int err = errno;
    return STATUS(NetworkError, std::string("write error: ") +
                                ErrnoToString(err), Slice(), err);
  }
  *nwritten = res;
  return Status::OK();
}

Status Socket::Writev(const struct ::iovec *iov, int iov_len,
                      int32_t *nwritten) {
  if (PREDICT_FALSE(iov_len <= 0)) {
    return STATUS(NetworkError,
                StringPrintf("writev: invalid io vector length of %d",
                             iov_len),
                Slice(), EINVAL);
  }
  DCHECK_GE(fd_, 0);

  struct msghdr msg;
  memset(&msg, 0, sizeof(struct msghdr));
  msg.msg_iov = const_cast<iovec *>(iov);
  msg.msg_iovlen = iov_len;
  int res = ::sendmsg(fd_, &msg, MSG_NOSIGNAL);
  if (PREDICT_FALSE(res < 0)) {
    int err = errno;
    return STATUS(NetworkError, std::string("sendmsg error: ") +
                                ErrnoToString(err), Slice(), err);
  }

  *nwritten = res;
  return Status::OK();
}

// Mostly follows writen() from Stevens (2004) or Kerrisk (2010).
Status Socket::BlockingWrite(const uint8_t *buf, size_t buflen, size_t *nwritten,
    const MonoTime& deadline) {
  DCHECK_LE(buflen, std::numeric_limits<int32_t>::max()) << "Writes > INT32_MAX not supported";
  DCHECK(nwritten);

  size_t tot_written = 0;
  while (tot_written < buflen) {
    int32_t inc_num_written = 0;
    int32_t num_to_write = buflen - tot_written;
    MonoDelta timeout = deadline.GetDeltaSince(MonoTime::Now());
    if (PREDICT_FALSE(timeout.ToNanoseconds() <= 0)) {
      return STATUS(TimedOut, "BlockingWrite timed out");
    }
    RETURN_NOT_OK(SetSendTimeout(timeout));
    Status s = Write(buf, num_to_write, &inc_num_written);
    tot_written += inc_num_written;
    buf += inc_num_written;
    *nwritten = tot_written;

    if (PREDICT_FALSE(!s.ok())) {
      // Continue silently when the syscall is interrupted.
      if (s.error_code() == EINTR) {
        continue;
      }
      if (s.error_code() == EAGAIN) {
        return STATUS(TimedOut, "");
      }
      return s.CloneAndPrepend("BlockingWrite error");
    }
    if (PREDICT_FALSE(inc_num_written == 0)) {
      // Shouldn't happen on Linux with a blocking socket. Maybe other Unices.
      break;
    }
  }

  if (tot_written < buflen) {
    return STATUS(IOError, "Wrote zero bytes on a BlockingWrite() call",
        StringPrintf("Transferred %zu of %zu bytes", tot_written, buflen));
  }
  return Status::OK();
}

Status Socket::Recv(uint8_t *buf, int32_t amt, int32_t *nread) {
  if (amt <= 0) {
    return STATUS(NetworkError,
          StringPrintf("invalid recv of %d bytes", amt), Slice(), EINVAL);
  }

  // The recv() call can return fewer than the requested number of bytes.
  // Especially when 'amt' is small, this is very unlikely to happen in
  // the context of unit tests. So, we provide an injection hook which
  // simulates the same behavior.
  if (PREDICT_FALSE(FLAGS_socket_inject_short_recvs && amt > 1)) {
    Random r(GetRandomSeed32());
    amt = 1 + r.Uniform(amt - 1);
  }

  DCHECK_GE(fd_, 0);
  int res = ::recv(fd_, buf, amt, 0);
  if (res <= 0) {
    if (res == 0) {
      return STATUS(NetworkError, "Recv() got EOF from remote", Slice(), ESHUTDOWN);
    }
    int err = errno;
    return STATUS(NetworkError, std::string("recv error: ") +
                                ErrnoToString(err), Slice(), err);
  }
  *nread = res;
  return Status::OK();
}

// Mostly follows readn() from Stevens (2004) or Kerrisk (2010).
// One place where we deviate: we consider EOF a failure if < amt bytes are read.
Status Socket::BlockingRecv(uint8_t *buf, size_t amt, size_t *nread, const MonoTime& deadline) {
  DCHECK_LE(amt, std::numeric_limits<int32_t>::max()) << "Reads > INT32_MAX not supported";
  DCHECK(nread);
  size_t tot_read = 0;

  // We populate this with the full (initial) duration of the timeout on the first iteration of the
  // loop below.
  MonoDelta full_timeout;

  while (tot_read < amt) {
    // Read at most the max value of int32_t bytes at a time.
    const int32_t num_to_read = std::min(amt - tot_read,
                                         static_cast<size_t>(std::numeric_limits<int32_t>::max()));
    const MonoDelta timeout = deadline.GetDeltaSince(MonoTime::Now());
    if (!full_timeout.Initialized()) {
      full_timeout = timeout;
    }
    if (PREDICT_FALSE(timeout.ToNanoseconds() <= 0)) {
      VLOG(4) << __func__ << " timed out in " << full_timeout.ToString();
      return STATUS(TimedOut, "");
    }
    RETURN_NOT_OK(SetRecvTimeout(timeout));
    int32_t inc_num_read = 0;
    Status s = Recv(buf, num_to_read, &inc_num_read);
    if (inc_num_read > 0) {
      tot_read += inc_num_read;
      buf += inc_num_read;
      *nread = tot_read;
    }

    if (PREDICT_FALSE(!s.ok())) {
      // Continue silently when the syscall is interrupted.
      //
      // We used to treat EAGAIN as a timeout, and the reason for that is not entirely clear
      // to me (mbautin). http://man7.org/linux/man-pages/man2/recv.2.html says that EAGAIN and
      // EWOULDBLOCK could be used interchangeably, and these could happen on a nonblocking socket
      // that no data is available on. I think we should just retry in that case.
      if (s.error_code() == EINTR || s.error_code() == EAGAIN) {
        continue;
      }
      return s.CloneAndPrepend("BlockingRecv error");
    }
    if (PREDICT_FALSE(inc_num_read == 0)) {
      // EOF.
      break;
    }
  }

  if (PREDICT_FALSE(tot_read < amt)) {
    return STATUS(IOError, "Read zero bytes on a blocking Recv() call",
        StringPrintf("Transferred %zu of %zu bytes", tot_read, amt));
  }
  return Status::OK();
}

Status Socket::SetTimeout(int opt, std::string optname, const MonoDelta& timeout) {
  if (PREDICT_FALSE(timeout.ToNanoseconds() < 0)) {
    return STATUS(InvalidArgument, "Timeout specified as negative to SetTimeout",
                                   timeout.ToString());
  }
  struct timeval tv;
  timeout.ToTimeVal(&tv);
  socklen_t optlen = sizeof(tv);
  if (::setsockopt(fd_, SOL_SOCKET, opt, &tv, optlen) == -1) {
    int err = errno;
    return STATUS(NetworkError,
        StringPrintf("Failed to set %s to %s", optname.c_str(), timeout.ToString().c_str()),
        ErrnoToString(err), err);
  }
  return Status::OK();
}

} // namespace yb
