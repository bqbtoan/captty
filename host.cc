#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <mp/dispatch.h>
#include "scoped_make_raw.h"
#include "ptyshell.h"
#include "unio.h"
#include "partty.h"

namespace Partty {

// FIXME 例外処理


class HostIMPL {
public:
	HostIMPL(int server_socket);
	int run(void);
private:
	int sh;
	int host;
	int server;
private:
	HostIMPL();
	HostIMPL(const HostIMPL&);
private:
	int io_stdin(int fd, short event);
	int io_server(int fd, short event);
	int io_shell(int fd, short event);
private:
	typedef mp::event<void> mpevent;
	typedef mp::dispatch<void> mpdispatch;
	mpdispatch mpdp;
	static const size_t SHARED_BUFFER_SIZE = 32 * 1024;
	char shared_buffer[SHARED_BUFFER_SIZE];
	static const int STDIN  = 0;
	static const int STDOUT = 1;
private:
	struct winsize winsz;
	static int get_window_size(int fd, struct winsize* ws);
	static int set_window_size(int fd, struct winsize* ws);
};


Host::Host(int server_socket) : impl(new HostIMPL(server_socket)) {}
Host::~Host() { delete impl; }

HostIMPL::HostIMPL(int server_socket) : server(server_socket) {}

int Host::run(void) { return impl->run(); }
int HostIMPL::run(void)
{
	ptyshell psh(STDIN);
	if( psh.fork(NULL) < 0 ) { pexit("psh.fork"); }
	sh = psh.masterfd();

	// makerawはPtyChildShellをforkした後
	PtyScopedMakeRaw makeraw(STDIN);

	if( fcntl(STDIN,  F_SETFL, O_NONBLOCK) < 0 ) { pexit("set stdin nonblock");  }
	if( fcntl(server, F_SETFL, O_NONBLOCK) < 0 ) { pexit("set server nonblock"); }
	if( fcntl(sh,     F_SETFL, O_NONBLOCK) < 0 ) { pexit("set sh nonblock");     }

	using namespace mp::placeholders;
	if( mpdp.add(STDIN,  mp::EV_READ, mp::bind(&HostIMPL::io_stdin, this, _1, _2)) < 0 ) {
		pexit("mpdp.add stdin");
	}
	if( mpdp.add(server, mp::EV_READ, mp::bind(&HostIMPL::io_server, this, _1, _2)) < 0 ) {
		pexit("mpdp.add server");
	}
	if( mpdp.add(sh,     mp::EV_READ, mp::bind(&HostIMPL::io_shell, this, _1, _2)) < 0 ) {
		pexit("mpdp.add sh");
	}

	get_window_size(STDIN, &winsz);

	return mpdp.run();
}


int HostIMPL::io_stdin(int fd, short event)
{
	ssize_t len = read(fd, shared_buffer, SHARED_BUFFER_SIZE);
	if( len < 0 ) {
		if( errno == EAGAIN || errno == EINTR ) {
			return 0;
		} else {
			pexit("read from stdin");
		}
	} else if( len == 0 ) {
		perror("end of stdin");
	}
	if( write_all(sh, shared_buffer, len) != (size_t)len ){
		pexit("write to sh");
	}
	// ウィンドウサイズを設定
	struct winsize next;
	get_window_size(fd, &next);
	if( winsz.ws_row != next.ws_row || winsz.ws_col != next.ws_col ) {
		set_window_size(fd, &next);
		winsz = next;
	}
	return 0;
}

int HostIMPL::io_server(int fd, short event)
{
	ssize_t len = read(fd, shared_buffer, SHARED_BUFFER_SIZE);
	if( len < 0 ) {
		if( errno == EAGAIN || errno == EINTR ) {
			return 0;
		} else {
			pexit("read from server");
		}
	} else if( len == 0 ) {
		perror("end of server");
		return 1;
	}
	if( write_all(sh, shared_buffer, len) != (size_t)len ){
		pexit("write to sh");
	}
	return 0;
}

int HostIMPL::io_shell(int fd, short event)
{
	ssize_t len = read(fd, shared_buffer, SHARED_BUFFER_SIZE);
	if( len < 0 ) {
		if( errno == EAGAIN || errno == EINTR ) {
			return 0;
		} else {
			pexit("read from sh");
		}
	} else if( len == 0 ) {
		perror("end of sh");
		return 1;
	}
	if( write_all(STDOUT, shared_buffer, len) != (size_t)len ) {
		pexit("write to stdout");
	}
	if( write_all(server, shared_buffer, len) != (size_t)len ) {
		pexit("write to server");
	}
	// 端末にバッファを書き込み終わるまで待つ
	tcdrain(STDOUT);
	return 0;
}

int HostIMPL::get_window_size(int fd, struct winsize* ws)
{
	return ioctl(fd, TIOCGWINSZ, ws);
}

int HostIMPL::set_window_size(int fd, struct winsize* ws)
{
	return ioctl(fd, TIOCSWINSZ, ws);
}


}  // namespace Partty

