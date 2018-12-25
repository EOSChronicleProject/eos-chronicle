// copyright defined in LICENSE.txt

#include <appbase/application.hpp>
#include "receiver_plugin.hpp"
#include "decoder_plugin.hpp"
#include <iostream>
#include <boost/exception/diagnostic_information.hpp>

using namespace appbase;

int main( int argc, char** argv ) {
  try {
    appbase::app().register_plugin<receiver_plugin>();
    if( !appbase::app().initialize<receiver_plugin>(argc, argv) )
      return -1;
    appbase::app().register_plugin<decoder_plugin>();
    if( !appbase::app().initialize<decoder_plugin>(argc, argv) )
      return -1;
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


