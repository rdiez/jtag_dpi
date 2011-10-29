
/* See the counterpart Verilog file jtag_dpi.v for more information
   on this module.

   At the moment, this implementation only runs on Linux.

   During development, use compiler flag -DDEBUG in order to enable assertions.
 
   About the socket protocol and possible alternatives.

     The protocol used over the socket between the adv_jtag_bridge and the DPI module
     could be optimised by allowing the client to queue complex commands, instead
     of repeatedly sending single bits in a bit-bang fashion. An example implementation 
     of this technique might be (I haven't actually looked into it) the
     "Embecosm cycle accurate SystemC JTAG interface", described in document
     "Using JTAG with SystemC - Implementation of a Cycle Accurate Interface".

     If the user is not really interested in debugging the JTAG support,
     but he just wants to debug software running on the OpenRISC core, it would be faster
     to skip the JTAG and TAP modules altogether and provide some direct
     link between the GDB protocol and the core's debugging system.

   About this socket protocol implementation:

     The current implementation polls the socket at least one per clock cycle.
     It would be faster to create a second thread to deal with the socket communications,
     or to use asynchronous I/O on the socket.

   License:
     
   Copyright (c) 2011 R. Diez                              
                                                             
   This source file may be used and distributed without        
   restriction provided that this copyright statement is not   
   removed from the file and that any derivative work contains 
   the original copyright notice and the associated disclaimer.
                                                             
   This source file is free software; you can redistribute it  
   and/or modify it under the terms of the GNU Lesser General  
   Public License version 3 as published by the Free Software Foundation.
                                                             
   This source is distributed in the hope that it will be      
   useful, but WITHOUT ANY WARRANTY; without even the implied  
   warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR     
   PURPOSE.  See the GNU Lesser General Public License for more
   details.                                                    
                                                             
   You should have received a copy of the GNU Lesser General   
   Public License along with this source; if not, download it  
   from http://www.gnu.org/licenses/
*/

#include <assert.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>

#include <stdexcept>
#include <sstream>


// We may have more error codes in the future, that's why the success value is zero.
// It would be best to return the error message as a string, but Verilog
// does not have good support for variable-length strings.
static const int RET_SUCCESS = 0;
static const int RET_FAILURE = 1;

static const char INFO_MSG_PREFIX[]       = "JTAG DPI module: ";
static const char ERROR_MSG_PREFIX_INIT[] = "Error initializing the JTAG DPI module: ";
static const char ERROR_MSG_PREFIX_TICK[] = "Error in the JTAG DPI module: ";

static bool s_already_initialized = false;

static uint16_t s_listening_tcp_port;
static int      s_listeningSocket;
static bool     s_listen_on_local_addr_only;

static bool s_print_informational_messages;
static bool s_listening_message_already_printed;


enum connection_state_enum
{
  cs_invalid,
  cs_waiting_to_receive_commands,
  cs_waiting_to_send_clock_notification
};

static int s_connectionSocket;
static connection_state_enum s_connectionState;

// The clock notification message provides an indication that at least
// the given number of ticks have elapsed since the last command
// that wrote data to the JTAG signals. Since the passing of time is also simulated,
// the client needs this indication in order to synchronise itself with the
// simulation's master clock. Otherwise, the client could send over the TCP/IP socket
// JTAG data faster than the simulated clock, and the simulation would miss JTAG signal changes.
static int s_jtag_tck_half_period_tick_count;
static const uint8_t CLOCK_NOTIFICATION_MSG = 0xFF;
static int s_clock_notification_counter;


static std::string get_error_message ( const char * const prefix_msg,
                                       const int errno_val )
{
  std::ostringstream str;
  
  if ( prefix_msg != NULL )
    str << prefix_msg;
  
  str << "Error code " << errno_val << ": ";

  char buffer[ 2048 ];

  #if (_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600) && ! _GNU_SOURCE
  #error "The call to strerror_r() below will not compile properly. The easiest thing to do is to define _GNU_SOURCE when compiling this module."
  #endif
  
  const char * const err_msg = strerror_r( errno_val, buffer, sizeof(buffer) );
  
  if ( err_msg == NULL )
  {
    str << "<no error message available>";
  }
  else
  {
    // Always terminate the string, just in case. Note that the string
    // may not actually be in the buffer, see the strerror_r() documentation.
    buffer[ sizeof( buffer ) / sizeof( buffer[0] ) - 1 ] = '\0';
    assert( strlen( buffer ) < sizeof( buffer ) );
    
    str << err_msg;
  }
  
  return str.str();
}


static void close_current_connection ( void )
{
  assert( s_connectionSocket != -1 );
  
  if ( -1 == close( s_connectionSocket ) )
    assert( false );
  
  s_connectionSocket = -1;
}


static void send_byte ( const uint8_t data )
{
  if ( -1 == send( s_connectionSocket,
                   &data,
                   sizeof(data),
                   0  // No special flags.
                   ) )
  {
    throw std::runtime_error( get_error_message( "Error sending data: ", errno ) );
  }
}


static std::string ip_address_to_text ( const in_addr * const addr )
{
  char ip_addr_buffer[80];
  
  const char * const str = inet_ntop( AF_INET,
                                      addr,
                                      ip_addr_buffer,
                                      sizeof(ip_addr_buffer) );
  if ( str == NULL )
  {
    throw std::runtime_error( get_error_message( "Error formatting the IP address: ", errno ) );
  }

  assert( strlen(str) <= strlen("123.123.123.123") );
  assert( strlen(str) <= sizeof(ip_addr_buffer) );

  return str;
}


static void close_listening_socket ( void )
{
  assert( s_listeningSocket != -1 );

  if ( -1 == close( s_listeningSocket ) )
    assert( false );

  s_listeningSocket = -1;
}


static void create_listening_socket ( void )
{
  assert( s_listeningSocket == -1 );

  s_listeningSocket = socket( PF_INET,
                              SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                              0 );

  if ( s_listeningSocket == -1 )
  {
    throw std::runtime_error( get_error_message( "Error creating the listening socket: ", errno ) );
  }

  try
  {
    // If this process terminates abruptly, the TCP/IP stack does not release
    // the listening ports immediately, at least under Linux (I've seen comments
    // about this issue under Windows too). Therefore, if you restart the simulation
    // whithin a few seconds, you'll get an annoying "address already in use" error message.
    // The SO_REUSEADDR prevents this from happening.
    const int set_reuse_to_yes = 1;
    if ( setsockopt( s_listeningSocket,
                     SOL_SOCKET,
                     SO_REUSEADDR,
                     &set_reuse_to_yes,
                     sizeof(set_reuse_to_yes) ) == -1 )
    {
      throw std::runtime_error( get_error_message( "Error setting the listen socket options: ", errno ) );
    }
    
    sockaddr_in addr;
    memset( &addr, 0, sizeof(addr) );
    addr.sin_family = AF_INET;
    addr.sin_port = htons( s_listening_tcp_port );
    addr.sin_addr.s_addr = ntohl( s_listen_on_local_addr_only ? INADDR_LOOPBACK : INADDR_ANY );

    if ( bind( s_listeningSocket,
               (struct sockaddr *)&addr,
               sizeof(addr) ) == -1 )
    {
      throw std::runtime_error( get_error_message( "Error binding the socket: ", errno ) );
    }

    // The listening IP address and listening port do not change, so print this information
    // only once at the beginning. Printing the message again just clutters
    // the screen with unnecessary information.
    if ( !s_listening_message_already_printed )
    {
      s_listening_message_already_printed = true;

      if ( s_print_informational_messages )
      {
        const std::string addr_str = ip_address_to_text( &addr.sin_addr );
        
        printf( "%sListening on IP address %s (%s), TCP port %d.\n",
                INFO_MSG_PREFIX,
                addr_str.c_str(),
                s_listen_on_local_addr_only ? "local only" : "all",
                s_listening_tcp_port );
        fflush( stdout );
      }
    }
  
    if ( listen( s_listeningSocket, 1 ) == -1 )
    {
      throw std::runtime_error( get_error_message( "Error listening on the socket: ", errno ) );
    }
  }
  catch ( ... )
  {
    close_listening_socket();
    throw;
  }
}


static void accept_connection ( void )
{
  assert( s_listeningSocket != -1 );

  pollfd polledFd;

  polledFd.fd      = s_listeningSocket;
  polledFd.events  = POLLIN | POLLERR;
  polledFd.revents = 0;

  const int pollRes = poll( &polledFd, 1, 0 );

  if ( pollRes == 0 )
  {
    // No incoming connection is yet there.
    return;
  }
  
  if ( pollRes == -1 )
  {
    throw std::runtime_error( get_error_message( "Error polling the listening socket: ", errno ) );
  }

  assert( pollRes == 1 );

  if ( s_print_informational_messages )
  {
    // printf( "%sPoll result flags: 0x%02X\n", polledFd.revents, INFO_MSG_PREFIX );
    // fflush( stdout );
  }
    
  sockaddr_in remoteAddr;
  socklen_t remoteAddrLen = sizeof( remoteAddr );

  const int connectionSocket = accept4( s_listeningSocket,
                                        (sockaddr *) &remoteAddr,
                                        &remoteAddrLen,
                                        SOCK_NONBLOCK | SOCK_CLOEXEC );

  // Any errors accepting a connection are considered non-critical and do not normally stop the simulation,
  // as the remote client can try to reconnect at a later point in time.
  try
  {
    if ( connectionSocket == -1 )
    {
      throw std::runtime_error( get_error_message( NULL, errno ) );
    }
    
    if ( remoteAddrLen > sizeof( remoteAddr ) )
    {
      throw std::runtime_error( "The address buffer is too small." );
    }

    if ( s_print_informational_messages )
    {
      const std::string addr_str = ip_address_to_text( &remoteAddr.sin_addr );
      
      printf( "%sAccepted an incoming connection from IP address %s, TCP port %d.\n",
              INFO_MSG_PREFIX,
              addr_str.c_str(),
              ntohs( remoteAddr.sin_port ) );
      fflush( stdout );
    }
  }
  catch ( const std::exception & e )
  {
    fprintf( stderr,
             "%sError accepting a connection on the listening socket: %s\n",
             ERROR_MSG_PREFIX_TICK,
             e.what() );
    fflush( stderr );
    
    if ( connectionSocket != -1 )
    {
      if ( -1 == close( connectionSocket ) )
        assert( false );
    }

    return;
  }

  s_connectionSocket = connectionSocket;
  s_connectionState  = cs_waiting_to_receive_commands;

  // If somebody else attempts to connect, he should get an error straight away.
  // However, if the listening socket is still active, the client will land in the accept queue
  // and he'll hopefully time-out eventually.
  close_listening_socket();
}


static void receive_commands ( unsigned char * const jtag_tms,
                               unsigned char * const jtag_tck,
                               unsigned char * const jtag_trst,
                               unsigned char * const jtag_tdi,
                               const unsigned char   jtag_tdo )
{
  for ( ; ; )
  {
    uint8_t received_data;

    const ssize_t received_byte_count = recv( s_connectionSocket,
                                              &received_data,
                                              1, // Receive just 1 byte.
                                              0  // No special flags.
                                            );
    if ( received_byte_count == 0 )
    {
      if ( s_print_informational_messages )
      {
        printf( "%sConnection closed at the other end.\n", INFO_MSG_PREFIX );
        fflush( stdout );
      }
      close_current_connection();
      break;
    }


    if ( received_byte_count == -1 )
    {
      if ( errno == EAGAIN || errno == EWOULDBLOCK )
      {
        // No data available yet.
        break;
      }

      throw std::runtime_error( get_error_message( "Error receiving data: ", errno ) );
    }

    assert( received_byte_count == 1 );

    if ( received_data & 0x80 )
    {
      if ( s_print_informational_messages )
      {
        // printf( "%sReceived JTAG command: 0x%02X\n", INFO_MSG_PREFIX, received_data );
        // fflush( stdout );
      }
      
      switch ( received_data )
      {
      case 0x80:
        send_byte( jtag_tdo ? 1 : 0 );
        break;

      case 0x81:
        if ( s_clock_notification_counter == 0 )
        {
          send_byte( CLOCK_NOTIFICATION_MSG );
        }
        else
        {
          s_connectionState = cs_waiting_to_send_clock_notification;
        }
        break;

      default:
        {
          char buffer[80];
          if ( int(sizeof(buffer)) <= sprintf( buffer, "Invalid command 0x%02X received.", received_data ) )
          {
            assert( false );
          }

          throw std::runtime_error( buffer );
        }
      }

      // We don't process new commands until the notification is due.
      // We could decide otherwise, but the current client does not need it,
      // so keep things simple.
      if ( s_connectionState == cs_waiting_to_send_clock_notification )
        break;
    }
    else
    {
      if ( s_print_informational_messages )
      {
        // printf( "%sReceived JTAG data 0x%02X.\n", INFO_MSG_PREFIX, received_data );
        // fflush( stdout );
      }

      if ( 0 != ( received_data & 0xf0 ) )
      {
        char buffer[80];
        if ( int(sizeof(buffer)) <= sprintf( buffer, "Invalid JTAG data byte 0x%02X received.", received_data ) )
        {
          assert( false );
        }
        throw std::runtime_error( buffer );
      }

      *jtag_tck  = ( received_data & 0x01 ) ? 1 : 0;
      *jtag_trst = ( received_data & 0x02 ) ? 1 : 0;
      *jtag_tdi  = ( received_data & 0x04 ) ? 1 : 0;
      *jtag_tms  = ( received_data & 0x08 ) ? 1 : 0;

      // Acknowledge the received data.
      send_byte( received_data | 0x10 );

      s_clock_notification_counter = s_jtag_tck_half_period_tick_count;
    }
  }
}


static void serve_connection ( unsigned char * const jtag_tms,
                               unsigned char * const jtag_tck,
                               unsigned char * const jtag_trst,
                               unsigned char * const jtag_tdi,
                               const unsigned char jtag_tdo )
{
  assert( s_connectionSocket != -1 );

  try
  {
    if ( s_clock_notification_counter > 0 )
      --s_clock_notification_counter;
    
    switch ( s_connectionState )
    {
    case cs_waiting_to_receive_commands:
      receive_commands( jtag_tms,
                        jtag_tck,
                        jtag_trst,
                        jtag_tdi,
                        jtag_tdo );
      break;
      
    case cs_waiting_to_send_clock_notification:

      if ( s_clock_notification_counter == 0 )
      {
        send_byte( CLOCK_NOTIFICATION_MSG );
        s_connectionState = cs_waiting_to_receive_commands;

        // In case there are already commands on the receive queue, process them right away.
        receive_commands( jtag_tms,
                          jtag_tck,
                          jtag_trst,
                          jtag_tdi,
                          jtag_tdo );
      }
      break;
      
    default:
      assert( false );
    }
  }
  catch ( const std::exception & e )
  {
    fprintf( stderr,
             "%sConnection closed after error: %s\n",
             ERROR_MSG_PREFIX_TICK,
             e.what() );
    fflush( stderr );
    
    // Close the connection. The remote client can reconnect later.
    close_current_connection();
  }
}


int jtag_dpi_init ( const int tcp_port,
                    const unsigned char listen_on_local_addr_only,
                    const int jtag_tck_half_period_tick_count,
                    const unsigned char print_informational_messages )
{
  try
  {
    if ( s_already_initialized )
    {
      throw std::runtime_error( "The module has already been initialized." );
    }
  
    if ( tcp_port == 0 )
    {
      throw std::runtime_error( "Invalid TCP port." );
    }
    
    s_listening_tcp_port = tcp_port;

    
    switch ( print_informational_messages )
    {
    case 0:
      s_print_informational_messages = false;
      break;
      
    case 1:
      s_print_informational_messages = true;
      break;

    default:
      throw std::runtime_error( "Invalid print_informational_messages parameter." );
    }

    
    switch ( listen_on_local_addr_only )
    {
    case 0:
      s_listen_on_local_addr_only = false;
      break;

    case 1:
      s_listen_on_local_addr_only = true;
      break;

    default:
      throw std::runtime_error( "Invalid listen_on_local_addr_only parameter." );
    }
    
    
    if ( jtag_tck_half_period_tick_count == 0 )
    {
      throw std::runtime_error( "Invalid jtag_tck_half_period_tick_count parameter." );
    }
    
    s_jtag_tck_half_period_tick_count = jtag_tck_half_period_tick_count;


    s_listeningSocket = -1;
    s_listening_message_already_printed = false;
    s_connectionSocket = -1;
    s_connectionState = cs_invalid;

    create_listening_socket();
    
    s_already_initialized = true;
  }
  catch ( const std::exception & e )
  {
    // We should return this error string to the caller,
    // but Verilog does not have good support for variable-length strings.
    fprintf( stderr, "%s%s\n", ERROR_MSG_PREFIX_INIT, e.what() );
    fflush( stderr );
    return RET_FAILURE;
  }
  catch ( ... )
  {
    fprintf( stderr, "%sUnexpected C++ exception.\n", ERROR_MSG_PREFIX_INIT );
    fflush( stderr );
    return RET_FAILURE;
  }

  return RET_SUCCESS;
}


int jtag_dpi_tick ( unsigned char * const jtag_tms,
                    unsigned char * const jtag_tck,
                    unsigned char * const jtag_trst,
                    unsigned char * const jtag_tdi,
                    const unsigned char jtag_tdo )
{
  try
  {
    if ( !s_already_initialized )
    {
      throw std::runtime_error( "This module has not been initialized yet." );
    }
    
    // If a connection is lost, the listening socket must be created again.
    
    if ( s_connectionSocket == -1 )
    {
      if ( s_listeningSocket == -1 )
      {
        create_listening_socket();
      }

      accept_connection();
    }

   if ( s_connectionSocket != -1 )
   {
     serve_connection( jtag_tms,
                       jtag_tck,
                       jtag_trst,
                       jtag_tdi,
                       jtag_tdo );
   }
  }
  catch ( const std::exception & e )
  {
    fprintf( stderr, "%s%s\n", ERROR_MSG_PREFIX_TICK, e.what() );
    fflush( stderr );
    return RET_FAILURE;
  }
  catch ( ... )
  {
    fprintf( stderr, "%sUnexpected C++ exception.\n", ERROR_MSG_PREFIX_TICK );
    fflush( stderr );
    return RET_FAILURE;
  }
  
  return RET_SUCCESS;
}


void jtag_dpi_terminate ( void )
{
    if ( !s_already_initialized )
    {
      // The user shouldn't call this routine if the module was not initialised,
      // although it does not really matter very much.
      assert( false );
      return;
    }

    if ( s_listeningSocket != -1 )
    {
      close_listening_socket();
    }

    if ( s_connectionSocket != -1 )
    {
      close_current_connection();
    }

    s_already_initialized = false;
}
