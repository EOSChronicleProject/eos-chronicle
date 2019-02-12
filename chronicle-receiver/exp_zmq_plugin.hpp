#include <appbase/application.hpp>
#include "chain_state_types.hpp"

using namespace appbase;

class exp_zmq_plugin : public appbase::plugin<exp_zmq_plugin>
{
public:
  APPBASE_PLUGIN_REQUIRES();
  exp_zmq_plugin();
  virtual ~exp_zmq_plugin();
  virtual void set_program_options(options_description& cli, options_description& cfg) override;  
  void plugin_initialize(const variables_map& options);
  void plugin_startup();
  void plugin_shutdown();
  
private:
  std::unique_ptr<class exp_zmq_plugin_impl> my;
};



