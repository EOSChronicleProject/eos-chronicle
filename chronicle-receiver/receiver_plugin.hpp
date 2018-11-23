#include <appbase/application.hpp>
#include <memory>

using namespace appbase;

class receiver_plugin : public appbase::plugin<receiver_plugin>
{
public:
  APPBASE_PLUGIN_REQUIRES();
  receiver_plugin();
  virtual ~receiver_plugin();
  virtual void set_program_options(options_description& cli, options_description& cfg) override;  
  void plugin_initialize(const variables_map& options);
  void plugin_startup();
  void plugin_shutdown();
private:
  std::unique_ptr<class receiver_plugin_impl> my;
};



