#include "partty.h"
#include <string>

namespace Partty {


class RawGateIMPL {
public:
	RawGateIMPL(RawGate::config_t& config);
	~RawGateIMPL();
	int run(void);
	void signal_end(void);
private:
	int accept_guest(void);
private:
	int socket;
	char gate_path[PATH_MAX + MAX_SESSION_NAME_LENGTH];
	size_t gate_dir_len;
#ifdef PARTTY_RAW_GATE_FLASH_CROSS_DOMAIN_SUPPORT
	std::string m_flash_cross_domain_policy;
#endif
	pid_t m_parent_pid;
	sig_atomic_t m_end;
private:
	RawGateIMPL();
	RawGateIMPL(const GateIMPL&);
private:
	static const int E_SUCCESS = 0;
	static const int E_ACCEPT = 1;
	static const int E_FINISH = 2;
	static const int E_READ = 3;
	static const int E_SESSION_NAME = 4;
	static const int E_PASSWORD = 5;
	static const int E_SENDFD = 6;
#ifdef PARTTY_RAW_GATE_FLASH_CROSS_DOMAIN_SUPPORT
	static const int E_FLASH_CROSS_DOMAIN = 7;
#endif
};


}  // namespace Partty

