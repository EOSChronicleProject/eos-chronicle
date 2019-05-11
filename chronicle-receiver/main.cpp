// copyright defined in LICENSE.txt

#include <appbase/application.hpp>
#include "receiver_plugin.hpp"
#include "decoder_plugin.hpp"
#include <iostream>
#include <boost/exception/diagnostic_information.hpp>

#include <fc/filesystem.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/log/appender.hpp>
#include <fc/exception/exception.hpp>

using namespace appbase;


void initialize_logging()
{
  const fc::path config_path(app().get_logging_conf());
  if (fc::exists(config_path)) {
    fc::configure_logging(config_path);
  }
}


int main( int argc, char** argv ) {
  try {
    appbase::app().register_plugin<receiver_plugin>();
    appbase::app().register_plugin<decoder_plugin>();
    if( !appbase::app().initialize<receiver_plugin, decoder_plugin>(argc, argv) )
      return -1;
    initialize_logging();
    appbase::app().startup();
    appbase::app().exec();
  } catch ( const boost::exception& e ) {
    std::cerr << boost::diagnostic_information(e) << "\n";
  } catch ( const std::exception& e ) {
    std::cerr << e.what() << "\n";
  } catch ( ... ) {
    std::cerr << "unknown exception\n";
  }
  std::cout << "exited cleanly\n";
  return 0;
}


