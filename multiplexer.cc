#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits>
#include "multiplexer.h"
#include "pty_make_raw.h"
#include "uniext.h"
#include "fdtransport.h"

#include <iostream>

namespace Partty {


// FIXME perror -> ログ

filt_telnetd::filt_telnetd() : emtelnet((void*)this)
{
	// use these options
	set_my_option_handler( emtelnet::OPT_SGA,
			filt_telnetd::pass_through_handler );
	set_my_option_handler( emtelnet::OPT_ECHO,
			filt_telnetd::pass_through_handler );
	set_my_option_handler( emtelnet::OPT_BINARY,
			filt_telnetd::pass_through_handler );

	// supported partner options
	set_partner_option_handler( emtelnet::OPT_BINARY,
			filt_telnetd::pass_through_handler );

	// prevent line mode
	send_will(emtelnet::OPT_SGA);
	send_will(emtelnet::OPT_ECHO);
	send_dont(emtelnet::OPT_ECHO);
	send_dont(emtelnet::OPT_LINEMODE);

	// enable multibyte characters
	send_will(emtelnet::OPT_BINARY);
	send_do(emtelnet::OPT_BINARY);
}


Multiplexer::Multiplexer(int host_socket, int gate_socket,
		const session_info_ref_t& info) :
	impl(new MultiplexerIMPL(host_socket, gate_socket, info)) {}
MultiplexerIMPL::MultiplexerIMPL(int host_socket, int gate_socket,
		const session_info_ref_t& info) :
	host(host_socket), gate(gate_socket),
	num_guest(0),
	m_info(info)
{
	// 監視対象のファイルディスクリプタにO_NONBLOCKをセット
	if( fcntl(host, F_SETFL, O_NONBLOCK) < 0 ) {
		throw initialize_error("failed to set host socket mode");
	}
	if( fcntl(gate, F_SETFL, O_NONBLOCK) < 0 ) {
		throw initialize_error("failed to set gate socket mode");
	}

	// mp::eventに登録
	if( mpev.add(host, mp::EV_READ) < 0 ) {
		throw initialize_error("can't add host socket to IO multiplexer");
	}
	if( mpev.add(gate, mp::EV_READ) < 0 ) {
		throw initialize_error("can't add gate socket to IO multiplexer");
	}

	// プロセス名を変更
	// session_nameに使われていない文字を探す
	int delimiter;
	char* exist;
	for(delimiter = 0x3A; delimiter <= 0x7E; ++delimiter) {
		// ':' - '~'
		if( (exist = strchr(info.session_name, delimiter)) ) { break; }
	}
	if( !exist ) {
		for(delimiter = 0x21; delimiter < 0x3A; ++delimiter) {
			// '!' - '9'
			if( (exist = strchr(info.session_name, delimiter)) ) { break; }
		}
	}
	if( !exist ) { delimiter = 255; }
	setprocname("partty-session%c %s%c%s", delimiter, info.session_name, delimiter, info.user_name);
}


Multiplexer::~Multiplexer() { delete impl; }
MultiplexerIMPL::~MultiplexerIMPL()
{
	// すべてのゲストをclose
	if( num_guest > 0 ) {
		int n = 0;
		int to = num_guest;
		for(int fd = 0; fd < INT_MAX; ++fd) {
			if( guest_set.test(fd) ) {
				close(fd);
				++n;
				if( n >= to ) { break; }
			}
		}
	}
}


int Multiplexer::run(void) { return impl->run(); }
int MultiplexerIMPL::run(void)
{
	int fd;
	short event;
	while(1) {
	// FIXME シグナルハンドラ
		while( mpev.next(&fd, &event) ) {
			if( fd == host ) {
				if( io_host(fd, event) < 0 ) { return 0; }
			} else if( fd == gate ) {
				if( io_gate(fd, event) < 0 ) { return 0; }
			} else {
				if( io_guest(fd, event, writable_guest[fd]) < 0 ) { return 0; }
			}
		}
		if( mpev.wait() < 0 ) {
			return -1;
		}
	}
	return 0;
}

int MultiplexerIMPL::io_gate(int fd, short event)
{
	// Gateからゲストのファイルディスクリプタを受け取る
	gate_message_t msg;
	int guest = recvfd(gate, &msg, sizeof(msg));
	if( guest < 0 ) {
		if( errno == EAGAIN || errno == EINTR ) { return 0; }
		else {
			perror("guest connection failed a");
			return 0;
		}
	}
static char trash[1024];
read(gate, trash, sizeof(trash));  // FIXME データがあった場合は捨てる？
	if( writable_guest.size() <= (writable_guest_t::size_type)guest ) {
		writable_guest.resize(guest+1);
	}
	// セッション名とパスワードをチェック
	if( msg.session_name.len != m_info.session_name_length ||
	    memcmp(msg.session_name.str,
		   m_info.session_name,
		   m_info.session_name_length) != 0 ) {
		// セッション名が不一致
		perror("session name not match");
		close(guest);
		return 0;
	}
	if( msg.password.len == m_info.writable_password_length &&
	    memcmp(msg.password.str,
		   m_info.writable_password,
		   m_info.writable_password_length) == 0 ) {
		// writable passwordが一致 -> writableゲスト
		writable_guest[guest] = true;
	} else if( msg.password.len == m_info.readonly_password_length &&
		   memcmp(msg.password.str,
			  m_info.readonly_password,
			  m_info.readonly_password_length) == 0 ) {
		// readonly passwordが一致 -> read-onlyゲスト
		writable_guest[guest] = false;
	} else {
		// 認証失敗
		perror("guest authentication failed");
		close(guest);
		return 0;
	}
	// ゲストを監視対象に追加
	if( fcntl(guest, F_SETFL, O_NONBLOCK) < 0 ||
			mpev.add(guest, mp::EV_READ | mp::EV_WRITE) < 0 ) {
			// イベント待ちは readable or writable から始める
		perror("guest connection failed");
		close(guest);
		return 0;
	}
	guest_set.set(guest);
	// 書き込み待ちバッファにPARTTY_SERVER_WELCOME_MESSAGEを加える
	guest_set.data(guest).send(PARTTY_SERVER_WELCOME_MESSAGE, strlen(PARTTY_SERVER_WELCOME_MESSAGE), NULL);
	num_guest++;
	return 0;
}

int MultiplexerIMPL::io_host(int fd, short event)
{
	// Host -> ゲスト
	// Hostからread
	ssize_t len = read(fd, shared_buffer, SHARED_BUFFER_SIZE);
	if( len < 0 ) {
		if( errno == EAGAIN || errno == EINTR ) { return 0; }
		else { throw io_error("host socket is broken"); }
	} else if( len == 0 ) {
		perror("read from host ends");
		return -1;
	}
	// len > 0
	// ゲストにwrite
	if( num_guest == 0 ) { return 0; }
	int n = 0;
	int to = num_guest;  // forの中でnum_guestが変動するので、ここで保存しておく
	for(int fd = 0; fd < INT_MAX; ++fd) {
		if( guest_set.test(fd) ) {
			// send_to_guestはすべてのデータを書き込めないかもしれない
			// すべて書き込めなければファイルディスクリプタをEV_WRITEで監視し、
			// io_guestで書き込む
			send_to_guest(fd, shared_buffer, len);  // ここでremove_guest()が実行されるかもしれない
			++n;
			if( n >= to ) { break; }
		}
	}
	return 0;
}

int MultiplexerIMPL::io_guest(int fd, short event, bool writable)
{
	if( event & mp::EV_READ ) {
		// ゲスト -> Host
		// ゲストからread
		ssize_t len = read(fd, shared_buffer, SHARED_BUFFER_SIZE);
		if( len < 0 ) {
			if( errno == EAGAIN || errno == EINTR ) { /* pass through */ }
			else {
				perror("read from guest failed");
				remove_guest(fd);
				return 0;
			}
		} else if( len == 0 ) {   // FIXME Mac OS X + SCM_RIGHTSはバグあり？
			perror("read from guest ends");
			remove_guest(fd);
			return 0;
		} else if( writable ) {
			// 書き込み可能guest -> host
			filt_telnetd::buffer_t ibuf;
			if( recv_filter(fd, shared_buffer, len, &ibuf) < 0 ) {
				// 切断されたゲストからのバッファは捨てる
				return 0;
			}
			// FIXME Hostが切断されたのかエラーが発生したのか区別できない
			if( continued_blocking_write_all(host, ibuf.buf, ibuf.len) != ibuf.len ) {
				throw io_error("host socket is broken");
			}
		}
		// read-only guestからの入力はそのまま捨てる
	}
	if( event & mp::EV_WRITE ) {
		// 書き込み待ちバッファ -> ゲスト
		filt_telnetd& srv( guest_set.data(fd) );
		ssize_t len = write(fd, srv.obuffer, srv.olength);
		//if( srv.olength == 0 ) { return 0; }  // olengthは0の可能性がある？
		if( len < 0 ) {
			if( errno == EAGAIN || errno == EINTR ) { return 0; }
			else {
				perror("write to guest failed");
				remove_guest(fd);
				return 0;
			}
		} else if( len == 0 ) {
			perror("write to guest ends");
			remove_guest(fd);
			return 0;
		} else {
			if( (size_t)len < srv.olength ) {
				// 途中まで書き込めた
				srv.oconsumed(len);
			} else {
				// 全部書き込めた
				// writable待ちを解除する
				if( mpev.modify(fd, mp::EV_READ | mp::EV_WRITE, mp::EV_READ) < 0 ) {
					perror("mpev.modify failed");
					remove_guest(fd);
					return 0;
				}
				srv.oclear();
			}
		}
	}
	return 0;
}

int MultiplexerIMPL::recv_filter(int fd, const void* buf, size_t len, filt_telnetd::buffer_t* ibuf)
{
	filt_telnetd& srv( guest_set.data(fd) );
	bool may_writable = srv.is_oempty();
	srv.recv(buf, len, ibuf, NULL);  // recvしたときにもobufが発生する
	srv.iclear();   // 呼び出し側はibufを確実に使い切らないといけない
	if( may_writable ) {
		// 書き込み待ちバッファが空だったので書き込めるかもしれない
		if( guest_try_write(fd, srv) < 0 ) {
			remove_guest(fd, srv);
			return -1;
		}
	}
	return 0;
}

int MultiplexerIMPL::send_to_guest(int fd, const void* buf, size_t len)
{
	filt_telnetd& srv( guest_set.data(fd) );
	bool may_writable = srv.is_oempty();
	srv.send(buf, len, NULL);
	if( may_writable ) {
		// 書き込み待ちバッファが空だったので書き込めるかもしれない
		if( guest_try_write(fd, srv) < 0 ) {
			remove_guest(fd, srv);
			return -1;
		}
	}
	return 0;
}

int MultiplexerIMPL::guest_try_write(int fd, filt_telnetd& srv)
{
	// 現在の書き込み待ちバッファにあるバッファを書き込んでみて、
	// 全部書き込めたらそのまま、書き込めなかったら書き込み待ちにする
	// 書き込みバッファが無かった状態から書き込み待ちバッファがある状態に
	// 遷移したときにのみ、この関数を呼べる
	if( srv.olength <= 0 ) { return 0; }
	ssize_t wlen = write(fd, srv.obuffer, srv.olength);
	if( wlen < 0 && errno != EAGAIN && errno != EINTR ) {
		perror("write to guest failed");
		return -1;
	} else if( wlen == 0 ) {
		perror("write to guest ends");
		return -1;
	}
	if( (size_t)wlen < srv.olength ) {
		// 全部書き込めなかった
		// バッファに残して書き込み待ちにする
		srv.oconsumed(wlen);
		if( mpev.modify(fd, mp::EV_READ, mp::EV_READ | mp::EV_WRITE) < 0 ) {
			perror("mpev.modify");
			return -1;
		}
	} else {
		// 全部書き込めた
		srv.oclear();
	}
	return 0;
}

void MultiplexerIMPL::remove_guest(int fd)
{
	filt_telnetd& srv( guest_set.data(fd) );
	remove_guest(fd, srv);
}

void MultiplexerIMPL::remove_guest(int fd, filt_telnetd& srv)
{
	mpev.remove(fd, mp::EV_READ | (srv.is_oempty() ? 0 : mp::EV_WRITE));
	guest_set.reset(fd);
	num_guest--;
	close(fd);
}


}  // namespace Partty

