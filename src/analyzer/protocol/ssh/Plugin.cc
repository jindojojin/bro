// See the file  in the main distribution directory for copyright.

namespace plugin {
namespace Bro_SSH {

class Plugin : public plugin::Plugin {
public:
	plugin::Configuration Configure()
		{
		AddComponent(new ::analyzer::Component("SSH", ::analyzer::ssh::SSH_Analyzer::Instantiate));

		plugin::Configuration config;
		config.name = "Bro::SSH";
		config.description = "Secure Shell analyzer";
		return config;
		}
} plugin;

}
}

