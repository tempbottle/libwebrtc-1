// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/unix_domain_server_socket_posix.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "base/logging.h"
#include "net/base/net_errors.h"
#include "net/socket/socket_libevent.h"
#include "net/socket/unix_domain_client_socket_posix.h"

namespace net {

namespace {

// Intended for use as SetterCallbacks in Accept() helper methods.
void SetStreamSocket(scoped_ptr<StreamSocket>* socket,
                     scoped_ptr<SocketLibevent> accepted_socket) {
  socket->reset(new UnixDomainClientSocket(accepted_socket.Pass()));
}

void SetSocketDescriptor(SocketDescriptor* socket,
                         scoped_ptr<SocketLibevent> accepted_socket) {
  *socket = accepted_socket->ReleaseConnectedSocket();
}

}  // anonymous namespace

UnixDomainServerSocket::UnixDomainServerSocket(
    const AuthCallback& auth_callback,
    bool use_abstract_namespace)
    : auth_callback_(auth_callback),
      use_abstract_namespace_(use_abstract_namespace) {
  DCHECK(!auth_callback_.is_null());
}

UnixDomainServerSocket::~UnixDomainServerSocket() {
}

// static
bool UnixDomainServerSocket::GetPeerCredentials(SocketDescriptor socket,
                                                Credentials* credentials) {
#if defined(OS_LINUX) || defined(OS_ANDROID)
  struct ucred user_cred;
  socklen_t len = sizeof(user_cred);
  if (getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &user_cred, &len) < 0)
    return false;
  credentials->process_id = user_cred.pid;
  credentials->user_id = user_cred.uid;
  credentials->group_id = user_cred.gid;
  return true;
#else
  return getpeereid(
      socket, &credentials->user_id, &credentials->group_id) == 0;
#endif
}

int UnixDomainServerSocket::Listen(const IPEndPoint& address, int backlog) {
  NOTIMPLEMENTED();
  return ERR_NOT_IMPLEMENTED;
}

int UnixDomainServerSocket::ListenWithAddressAndPort(
    const std::string& unix_domain_path,
    uint16 port_unused,
    int backlog) {
  DCHECK(!listen_socket_);

  SockaddrStorage address;
  if (!UnixDomainClientSocket::FillAddress(unix_domain_path,
                                           use_abstract_namespace_,
                                           &address)) {
    return ERR_ADDRESS_INVALID;
  }

  scoped_ptr<SocketLibevent> socket(new SocketLibevent);
  int rv = socket->Open(AF_UNIX);
  DCHECK_NE(ERR_IO_PENDING, rv);
  if (rv != OK)
    return rv;

  rv = socket->Bind(address);
  DCHECK_NE(ERR_IO_PENDING, rv);
  if (rv != OK) {
    PLOG(ERROR)
        << "Could not bind unix domain socket to " << unix_domain_path
        << (use_abstract_namespace_ ? " (with abstract namespace)" : "");
    return rv;
  }

  rv = socket->Listen(backlog);
  DCHECK_NE(ERR_IO_PENDING, rv);
  if (rv != OK)
    return rv;

  listen_socket_.swap(socket);
  return rv;
}

int UnixDomainServerSocket::GetLocalAddress(IPEndPoint* address) const {
  NOTIMPLEMENTED();
  return ERR_NOT_IMPLEMENTED;
}

int UnixDomainServerSocket::Accept(scoped_ptr<StreamSocket>* socket,
                                   const CompletionCallback& callback) {
  DCHECK(socket);

  SetterCallback setter_callback = base::Bind(&SetStreamSocket, socket);
  return DoAccept(setter_callback, callback);
}

int UnixDomainServerSocket::AcceptSocketDescriptor(
    SocketDescriptor* socket,
    const CompletionCallback& callback) {
  DCHECK(socket);

  SetterCallback setter_callback = base::Bind(&SetSocketDescriptor, socket);
  return DoAccept(setter_callback, callback);
}

int UnixDomainServerSocket::DoAccept(const SetterCallback& setter_callback,
                                     const CompletionCallback& callback) {
  DCHECK(!setter_callback.is_null());
  DCHECK(!callback.is_null());
  DCHECK(listen_socket_);
  DCHECK(!accept_socket_);

  while (true) {
    int rv = listen_socket_->Accept(
        &accept_socket_,
        base::Bind(&UnixDomainServerSocket::AcceptCompleted,
                   base::Unretained(this),
                   setter_callback,
                   callback));
    if (rv != OK)
      return rv;
    if (AuthenticateAndGetStreamSocket(setter_callback))
      return OK;
    // Accept another socket because authentication error should be transparent
    // to the caller.
  }
}

void UnixDomainServerSocket::AcceptCompleted(
    const SetterCallback& setter_callback,
    const CompletionCallback& callback,
    int rv) {
  if (rv != OK) {
    callback.Run(rv);
    return;
  }

  if (AuthenticateAndGetStreamSocket(setter_callback)) {
    callback.Run(OK);
    return;
  }

  // Accept another socket because authentication error should be transparent
  // to the caller.
  rv = DoAccept(setter_callback, callback);
  if (rv != ERR_IO_PENDING)
    callback.Run(rv);
}

bool UnixDomainServerSocket::AuthenticateAndGetStreamSocket(
    const SetterCallback& setter_callback) {
  DCHECK(accept_socket_);

  Credentials credentials;
  if (!GetPeerCredentials(accept_socket_->socket_fd(), &credentials) ||
      !auth_callback_.Run(credentials)) {
    accept_socket_.reset();
    return false;
  }

  setter_callback.Run(accept_socket_.Pass());
  return true;
}

}  // namespace net
