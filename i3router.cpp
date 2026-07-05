/*
 * Intermud-3 Router Copyright (c)2003 by Roger Libiez (Samson)
 *
 * Version 2.0
 *
 * See License/License.txt for legal info.
 */

/*
 * Uncomment this to enable Noplex's packet verification.
 * If not enabled, channel-admin packets will not be properly processed.
 * However, with this enabled, packets can often get mangled beyond use.
 * So only enable this if you're trying to help debug why channel-admin
 * isn't working properly.
 */
#define PACKET_CHECK

#include <fcntl.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <list>
#include <random>
#include <thread>
#include "i3router.h"

/*
 * This will seed the random number generator once during startup. I guess it's magic code :P
 * Uses the Mersenne Twister algorithm. - Samson 7/2/2026.
 */
std::mt19937 global_rng( std::random_device{}() );

std::list<descriptor_data *> descriptor_list;
std::list<ban_data *> ban_list;
std::list<i3_mud *> mud_list;
std::list<event_info *> eventlist;
std::list<i3_channel *> channel_list;
std::string I3_THISMUD;
std::string I3_THISIP;

int I3_socket = -1;
bool router_down = false;
long bytes_received = 0;
long bytes_sent = 0;
bool packetdebug = false; /* Packet debugging toggle, can be turned on to check outgoing packets */
std::chrono::system_clock::time_point current_time;

i3_mud *this_mud;
const char *alarm_section = "(unknown)";
int newdesc = -1;
int mud_count = 0;
int chan_count = 0;
long events_served = 0;

void close_socket( descriptor_data *dclose );
void i3_sendtoall( i3_mud *mud, std::string_view type, std::string_view msg );

descriptor_data::descriptor_data( )
{
}

descriptor_data::~descriptor_data( )
{
   close( this->descriptor );
   descriptor_list.remove( this );
}

event_info::event_info( )
{
}

event_info::~event_info( )
{
}

i3header::i3header( )
{
}

i3header::~i3header( )
{
}

ban_data::ban_data( )
{
}

ban_data::~ban_data( )
{
   ban_list.remove( this );
}

i3_mud::i3_mud( )
{
}

i3_mud::~i3_mud( )
{
   mud_list.remove( this );
}

i3_listener::i3_listener( )
{
}

i3_listener::~i3_listener( )
{
}

mlist::mlist( )
{
}

mlist::~mlist( )
{
}

i3_channel::i3_channel( )
{
}

i3_channel::~i3_channel( )
{
   for( auto it = this->listeners.begin(  ); it != this->listeners.end(  ); )
   {
      i3_listener *listener = *it;
      ++it;

      this->listeners.remove( listener );
      deleteptr( listener );
   }

   for( auto it = this->m_listeners.begin(  ); it != this->m_listeners.end(  ); )
   {
      mlist *m_list = *it;
      ++it;

      this->m_listeners.remove( m_list );
      deleteptr( m_list );
   }

   this->listeners.clear();
   this->m_listeners.clear();

   channel_list.remove( this );
}

// This only exists to correct a legit mistake on the part of the C++ committee. Boolean values are not WORDS, they are NUMBERS. Treat them as such.
template <>
struct std::formatter<bool> : std::formatter<int>
{
   auto format(bool b, format_context& ctx) const { return std::formatter<int>::format( b ? 1 : 0, ctx ); }
};

const std::string c_time( std::chrono::system_clock::time_point curtime )
{
   const std::chrono::time_zone* zone = std::chrono::current_zone();

   std::chrono::zoned_time zt{zone, std::chrono::floor<std::chrono::seconds>(curtime)};

   // Format: Sun Jan 01, 2026 12:00:00 PM UTC
   // %I is 12-hour clock, %p is AM/PM, %Z is zone name.
   return std::format( "{:%a %b %d, %Y %I:%M:%S %p %Z}", zt );
}

// Historical compatibility: Returns FALSE when they match, TRUE when they don't.
bool str_cmp( std::string_view astr, std::string_view bstr )
{
   // If neither one exists, then they're equal.
   if( astr.empty() && bstr.empty() )
      return false;

   auto case_insensitive_equals = []( std::string_view a, std::string_view b ) {
      return std::ranges::equal(a, b, [](char c1, char c2) {
         return std::tolower( static_cast<unsigned char>(c1) ) == std::tolower( static_cast<unsigned char>(c2) );
      });
   };

   if( case_insensitive_equals( astr, bstr ) )
      return false; // They match.
   return true; // They do not match.
}

// Strips off any leading and trailing spaces, plus any stray tabs, carriage returns, or newlines.
void strip_whitespace( std::string & str )
{
   // This should be every conceivable whitespace character to run into.
   const std::string_view whitespace = " \t\r\n\v\f";

   // Find first non-whitespace character.
   const auto start = str.find_first_not_of( whitespace );
   if( start == std::string::npos )
   {
      str.clear(); // The string is entirely whitespace.
      return;
   }

   // Find last non-whitespace character.
   const auto end = str.find_last_not_of( whitespace );

   // Update the string.
   str = str.substr( start, end - start + 1 );
}

template <typename... Args>
void i3log( std::format_string<Args...> fmt, Args&&... args )
{
   std::cerr << std::format( "{} :: ", c_time( current_time ) );
   std::cerr << std::format( fmt, std::forward<Args>( args )... );
   std::cerr << '\n';
}

template <typename... Args>
void i3bug( std::format_string<Args...> fmt, Args&&... args )
{
   std::cerr << std::format( "{} :: *** BUG: ", c_time( current_time ) );
   std::cerr << std::format( fmt, std::forward<Args>( args )... );
   std::cerr << '\n';
}

void write_shutdown_file( )
{
   std::ofstream stream( std::filesystem::path{"shutdown.txt"} );
   if( !stream.is_open() )
   {
      i3bug( "{}: Cannot open shutdown.txt for writing: {}", __func__, std::strerror(errno) );
      return;
   }
   stream << "Router shutdown was called.\n";
   stream.close();
   if( stream.fail() )
      i3bug( "{}: Error occurred after closing shutdown.txt: {}", __func__, std::strerror(errno) );
}

void free_event( event_info * e )
{
   eventlist.remove( e );
   deleteptr( e );
}

void free_all_events( void )
{
   for( auto it = eventlist.begin(  ); it != eventlist.end(  ); )
   {
      event_info *ev = *it;
      ++it;

      free_event( ev );
   }
}

void add_event( time_t when, void ( *callback ) ( void * ), void *data )
{
   event_info *e = new event_info;

   e->when = current_time + std::chrono::seconds( when );
   e->callback = callback;
   e->data = data;

   for( auto it = eventlist.begin(); it != eventlist.end(); ++it )
   {
      event_info *cur = *it;

      if( cur->when > e->when )
      {
         eventlist.insert( it, e );
         return;
      }
   }
   eventlist.push_back( e );
}

void run_events( std::chrono::system_clock::time_point newtime )
{
   while( !eventlist.empty(  ) )
   {
      event_info *e = eventlist.front();

      if( e->when > newtime )
         break;

      auto callback = e->callback;
      void *data = e->data;

      // Temporarily changed in case the callback function needs to know if the event matches current real world time.
      current_time = e->when;

      eventlist.pop_front();
      ++events_served;

      if( callback )
         callback ( data );
      else
         i3bug( "{}: nullptr callback", __func__ );

      deleteptr (e );
   }
   current_time = newtime;
}

void cancel_event( void ( *callback ) ( void * ), void *data )
{
   for( auto it = eventlist.begin(  ); it != eventlist.end(  ); )
   {
      event_info *ev = *it;
      ++it;

      if( ( !callback ) && ev->data == data )
         free_event( ev );

      else if( ( callback ) && ev->data == data && data != nullptr )
         free_event( ev );

      else if( ev->callback == callback && data == nullptr )
         free_event( ev );
   }
}

event_info *find_event( void ( *callback ) ( void * ), void *data )
{
   for( auto* ev : eventlist )
   {
      if( ev->callback == callback && ev->data == data )
         return ev;
   }
   return nullptr;
}

// Read one full line from a file, stopping at the first instance of the delimiter.
std::string i3fread_line( std::ifstream & stream, char delimiter )
{
   std::string line;

   if( std::getline( stream, line, delimiter ) )
      strip_whitespace( line ); // Once you have the line, it's best to strip it of any potential whitespace characters to eliminate the possibility of a bloat loop later on when the value is written back to disk.

   return line;
}

// Read one word from a file. Can be encased in single or double quotes.
std::string i3fread_word( std::ifstream & stream )
{
   char c;

   // Skip leading whitespace.
   while( stream.get(c) && std::isspace( static_cast<unsigned char>(c) ) );

   if( stream.eof() )
   {
      i3bug( "{}: EOF encountered on read.", __func__ );
      return {};
   }

   std::string word;
   word.reserve(64);

   if( c == '\'' || c == '"' )
   {
      const char delimiter = c;

      while( stream.get(c) && c != delimiter )
      {
         word.push_back(c);
      }

      if( stream.eof() )
      {
         i3bug( "{}: EOF encountered inside quoted string.", __func__ );
      }
      return word;
   }

   word.push_back(c);
   while( stream.get(c) )
   {
      if( std::isspace( static_cast<unsigned char>(c) ) )
      {
         stream.unget();
         break;
      }
      word.push_back(c);
   }
   return word;
}

// Read a letter from a file.
char i3fread_letter( std::ifstream & stream )
{
   char c;

   while( stream.get(c) )
   {
      if( !std::isspace( static_cast<unsigned char>( c ) ) )
      {
         return static_cast<char>( c );
      }
   }

   i3bug( "{}: EOF encountered on read.", __func__ );
   return '\0';
}

// Read to end of line (for comments).
void i3fread_to_eol( std::ifstream & stream )
{
   stream.ignore( static_cast<std::streamsize>(MSL), '\n' );
}

// Searches through the channel list to see if one exists with the I3 channel name supplied to it.
i3_channel *find_channel( std::string_view name )
{
   for( auto *channel :channel_list )
   {
      if( !str_cmp( channel->name, name ) )
         return channel;
   }
   return nullptr;
}

i3_mud *find_mud( std::string_view name )
{
   for( auto* mud: mud_list )
   {
      if( !str_cmp( name, mud->name ) )
         return mud;
   }
   return nullptr;
}

// Add backslashes in front of the " and \'s
std::string I3_escape( std::string_view input )
{
   std::string escaped;
   escaped.reserve( input.size() * 2 );

   for( char c : input )
   {
      if( c == '"' || c == '\\' )
      {
         escaped.push_back( '\\' );
      }
      escaped.push_back(c);
   }
   return escaped;
}

/*
 * Gets the next I3 field, that is when the amount of {[("'s and
 * ")]}'s match each other when a , is read. It's not foolproof, it
 * should honestly be some kind of statemachine, which does error-
 * checking. Right now I trust the I3-router to send proper packets
 * only. How naive :-) [Indeed Edwin, but I suppose we have little choice :P - Samson]
 *
 * ps will point to the beginning of the next field.
 *
 */
char *I3_get_field( char *packet, char **ps )
{
   int count[256];
   char has_apostrophe = 0, has_backslash = 0;
   char foundit = 0;

   bzero( count, sizeof(count) );

   *ps = packet;

   while( 1 ) 
   {
      switch( *ps[0] )
      {
         case '{': if( !has_apostrophe ) count[(int)'{']++; break;
         case '}': if( !has_apostrophe ) count[(int)'}']++; break;
         case '[': if( !has_apostrophe ) count[(int)'[']++; break;
         case ']': if( !has_apostrophe ) count[(int)']']++; break;
         case '(': if( !has_apostrophe ) count[(int)'(']++; break;
         case ')': if( !has_apostrophe ) count[(int)')']++; break;
         case '\\':
            if( has_backslash )
               has_backslash = 0;
            else
               has_backslash = 1;
            break;
         case '"':
            if( has_backslash )
            {
               has_backslash = 0;
            }
            else
            {
               if( has_apostrophe )
                  has_apostrophe = 0;
               else
                  has_apostrophe = 1;
            }
            break;
         case ',':
         case ':':
            if( has_apostrophe )
               break;
            if( has_backslash )
               break;
            if( count[(int)'{'] != count[(int)'}'] )
               break;
            if( count[(int)'['] != count[(int)']'] )
               break;
            if( count[(int)'('] != count[(int)')'] )
               break;
            foundit = 1;
            break;
         default:
            break;
      }
      if( foundit )
         break;
      (*ps)++;
   }
   *ps[0] = '\0';
   (*ps)++;
   return *ps;
}

/*
 * Remove "'s at begin/end of string
 * If a character is prefixed by \'s it also will be unescaped
 */
void I3_remove_quotes( char **ps ) 
{
   char *ps1, *ps2;

   if( *ps[0] == '"' )
      (*ps)++;
   if( (*ps)[strlen(*ps)-1] == '"' )
      (*ps)[strlen(*ps)-1] = '\0';

   ps1 = ps2 = *ps;
   while( ps2[0] ) 
   {
      if( ps2[0] == '\\' )
      {
         ps2++;
      }
      ps1[0] = ps2[0];
      ps1++;
      ps2++;
   }
   ps1[0] = '\0';
}

/*
 * Read the header of an I3 packet. pps will point to the next field
 * of the packet.
 */
i3header *I3_get_header( char **pps )
{
   char *ps = *pps, *next_ps;

   i3header *header = new i3header;

   I3_get_field( ps, &next_ps );
   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( header->originator_mudname, ps, 256 );
   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( header->originator_username, ps, 256 );
   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( header->target_mudname, ps, 256 );
   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( header->target_username, ps, 256 );
   *pps = next_ps;

   return header;
}

// Writes the string into the socket, prefixed by the size.
bool I3_write_packet( descriptor_data *d, std::string_view msg )
{
   if( msg.empty() )
      return true;

   uint32_t len = htonl( static_cast<uint32_t>( msg.size() ) );
   int sbytes = 0;
   size_t offset = 0;

   std::string packet;
   packet.reserve( 4 + msg.size() );
   packet.append( reinterpret_cast<const char*>(&len), 4 );
   packet.append( msg );

   while( offset < packet.size() )
   {
      size_t nBlock = std::min( packet.size() - offset, size_t{4096} );
      int nWrite = send( d->descriptor, packet.data() + offset, static_cast<int>(nBlock), 0 );

      if( nWrite == -1 )
      {
         if( errno == EWOULDBLOCK || errno == EAGAIN )
         {
            d->output_buffer.insert( 0, packet.substr(offset) );
            return true; // Partially sent, not a fatal error
         }
         return false;
      }

      sbytes += nWrite;
      offset += nWrite;
   }

   bytes_sent += sbytes;

   if( packetdebug )
   {
      i3log( "Bytes Sent: {}", sbytes );
      i3log( "Packet Sent: {}", msg );
   }

   d->output_buffer.erase();
   return true;
}

void I3_send_packet( descriptor_data *d )
{
   if( d == nullptr )
      return;

   if( !I3_write_packet( d, d->output_buffer ) )
      i3log( "Unable to write to descriptor." );
}

// Write a string into the send-buffer. Does not yet send it.
void I3_write_buffer( i3_mud *mud, std::string_view msg )
{
   if( !mud->desc )
      return;

   mud->desc->output_buffer.append( msg );
}

// Put a I3-header in the send-buffer. If a field is empty it will be replaced by a 0 (zero).
void I3_write_header( i3_mud *mud, std::string_view identifier, std::string_view originator_mudname, std::string_view originator_username, std::string_view target_mudname, std::string_view target_username )
{
   I3_write_buffer( mud, "({\"" );
   I3_write_buffer( mud, identifier );
   I3_write_buffer( mud, "\",5," );
   if( !originator_mudname.empty() && str_cmp( originator_mudname, "0" ) )
   {
      I3_write_buffer( mud, "\"" );
      I3_write_buffer( mud, originator_mudname );
      I3_write_buffer( mud, "\"," );
   }
   else I3_write_buffer( mud, "0," );

   if( !originator_username.empty() && str_cmp( originator_username, "0" ) )
   {
      I3_write_buffer( mud, "\"" );
      I3_write_buffer( mud, originator_username );
      I3_write_buffer( mud, "\"," );
   }
   else I3_write_buffer( mud, "0," );

   if( !target_mudname.empty() && str_cmp( target_mudname, "0" ) )
   {
      I3_write_buffer( mud, "\"" );
      I3_write_buffer( mud, target_mudname );
      I3_write_buffer( mud, "\"," );
   }
   else I3_write_buffer( mud, "0," );

   if( !target_username.empty() && str_cmp( target_username, "0" ) )
   {
      I3_write_buffer( mud, "\"" );
      I3_write_buffer( mud, target_username );
      I3_write_buffer( mud, "\"," );
   }
   else I3_write_buffer( mud, "0," );
}

void I3_send_error( i3_mud *mud, std::string_view user, std::string_view code, std::string_view message, std::string_view packet )
{
   I3_write_header( mud, "error", I3_THISMUD, "", mud->name, user );
   I3_write_buffer( mud, "\"" );
   I3_write_buffer( mud, code );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, I3_escape( message ) );
   I3_write_buffer( mud, "\"," );
   if( packet.empty() || !str_cmp( packet, "0" ) )
      I3_write_buffer( mud, "0," );
   else
   {
      I3_write_buffer( mud, packet );
      I3_write_buffer( mud, "," );
   }
   I3_write_buffer( mud, "})\r" );
}

int random_number( int from, int to )
{
   if( from > to )
      std::swap( from, to );
   if( from == to )
      return from;

   static std::uniform_int_distribution<int> dist;
   using param_t = std::uniform_int_distribution<int>::param_type;
   return dist( global_rng, param_t( from, to ) );
}

void I3_fread_config_file( std::ifstream & stream )
{
   std::string key;
   while( stream >> key )
   {
      if( key[0] == '*' )
      {
         i3fread_to_eol( stream );
         continue;
      }
      else if( key == "Router" )
         this_mud->routerIP = i3fread_line( stream, '\n' );
      else if( key == "Port" )
         stream >> this_mud->player_port;
      else if( key == "Routername" )
         this_mud->routerName = i3fread_line( stream, '\n' );
      else if( key == "End" )
         return;
      else
      {
         i3bug( "{}: Bad section '{}' in config file - skipping.", __func__, key );
         i3fread_to_eol( stream );
      }
   }
}

bool I3_read_config( void ) 
{
   i3log( "Loading Intermud-3 network data..." );

   std::ifstream stream( std::filesystem::path{CONFIG_FILE} );
   if( !stream.is_open() )
   {
      i3bug( "{}: Cannot open {} for reading: {}", __func__, CONFIG_FILE, std::strerror(errno) );
      return false;
   }

   if( !this_mud )
      this_mud = new i3_mud;

   this_mud->player_port = 0;

   std::string key;
   while( stream >> key )
   {
      if( key == "#I3CONFIG" )
         I3_fread_config_file( stream );
      else if( key == "#END" )
         break;
      else
      {
         i3bug( "{}: Bad section '{}' in {} - skipping.", __func__, key, CONFIG_FILE );
         i3fread_to_eol( stream );
      }
   }
   stream.close();

   if( this_mud->routerIP.empty() )
   {
      i3log( "Router IP not loaded in configuration file." );
      i3log( "Network configuration aborted." );
      return false;
   }

   if( !this_mud->player_port )
   {
      i3log( "Router port not loaded in configuration file." );
      i3log( "Network configuration aborted." );
      return false;
   }

   if( this_mud->routerName.empty() )
   {
      i3log( "No router name loaded in config file." );
      i3log( "Network configuration aborted." );
      return false;
   }

   I3_THISMUD = this_mud->routerName;
   I3_THISIP = this_mud->routerIP;

   i3log( "IP Address: {}", I3_THISIP );
   i3log( "Network data loaded." );
   return true;
}

void I3_readchannel( i3_channel *channel, std::ifstream & stream )
{
   std::string key;
   while( stream >> key )
   {
      if( key == "ChanName" )
         channel->name = i3fread_line( stream, '\n' );
      else if( key == "ChanMud" )
         channel->host_mud = i3fread_line( stream, '\n' );
      else if( key == "ChanStatus" )
         stream >> channel->status;
      else if( key =="Purgetime" )
      {
         time_t loaded_time;
         stream >> loaded_time;

         channel->purge_time = std::chrono::system_clock::from_time_t( loaded_time );
      }
      else if( key == "Mlist" )
      {
         mlist *m_list = new mlist;

         m_list->name = i3fread_line( stream, '\n' );
         channel->m_listeners.push_back( m_list );
      }
      else if( key == "End" )
         return;
      else
      {
         i3bug( "{}: Bad section '{}' - skipping.", __func__, key );
         i3fread_to_eol( stream );
      }
   }
}

void I3_loadchannels( void )
{
   i3log( "Loading channels..." );

   std::ifstream stream( std::filesystem::path{CHANNEL_FILE} );
   if( !stream.is_open() )
   {
      i3bug( "{}: Cannot open {} for reading: {}", __func__, CHANNEL_FILE, std::strerror(errno) );
      return;
   }

   chan_count = 0;

   std::string key;
   while( stream >> key )
   {
      if( key == "#I3CHAN" )
      {
         i3_channel *channel = new i3_channel;
         channel->purge_time = current_time + std::chrono::days( 30 );

         I3_readchannel( channel, stream );

         if( channel->purge_time <= std::chrono::system_clock::time_point{} )
            channel->purge_time = current_time + std::chrono::days( 30 );
         channel_list.push_back( channel );

         if( !channel->host_mud.empty() && ( channel->status == 1 || channel->status == 2 ) )
         {
            bool found = false;
            for( auto* m_list : channel->m_listeners )
            {
               if( !str_cmp( m_list->name, channel->host_mud ) )
               {
                  found = true;
                  break;
               }
            }

            if( !found )
            {
               mlist *m_list = new mlist;
               m_list->name = channel->host_mud;
               channel->m_listeners.push_back( m_list );
            }
         }
         chan_count++;
         continue;
      }
      else if( key == "#END" )
         break;
      else
      {
         i3bug( "{}: Bad section '{}' in {} - skipping.", __func__, key, CHANNEL_FILE );
         i3fread_to_eol( stream );
      }
   }
   stream.close();
   i3log( "{} Channels loaded.", chan_count );
}

void save_channels( void )
{
   std::ofstream stream( std::filesystem::path{CHANNEL_FILE} );
   if( !stream.is_open() )
   {
      i3bug( "{}: Cannot open {} for writing: {}", __func__, CHANNEL_FILE, std::strerror(errno) );
      return;
   }

   for( auto* channel : channel_list )
   {
      auto purge_time = std::chrono::system_clock::to_time_t( channel->purge_time );

      stream << "#I3CHAN\n";
      stream << std::format( "ChanName   {}\n", channel->name );
      stream << std::format( "ChanMud    {}\n", channel->host_mud );
      stream << std::format( "ChanStatus {}\n",  channel->status );
      stream << std::format( "Purgetime  {}\n", purge_time );

      for( auto* m_list : channel->m_listeners )
         stream << std::format( "Mlist      {}\n", m_list->name );
      stream << "End\n\n";
   }
   stream << "#END\n";
   stream.close();
   if( stream.fail() )
      i3bug( "{}: Error occurred after closing {}: ", __func__, CHANNEL_FILE, std::strerror(errno) );
}

void ev_savechanlist( void *data )
{
   save_channels();
   add_event( 300, ev_savechanlist, nullptr );
}

void send_chanlist( i3_mud *mud )
{
   /*
    * Muds crash if there are no channels. Empty chanlist packets are evil anyway.
    * chanlist-reply also isn't required by the protocol to complete connection with.
    */
   if( channel_list.empty() )
      return;

   mud->chanlist_id++;
   I3_write_header( mud, "chanlist-reply", I3_THISMUD, "", mud->name, "" );
   std::string s = std::format( "{},([", mud->chanlist_id );
   I3_write_buffer( mud, s );

   for( auto* chan : channel_list )
   {
      s = std::format( "\"{}\":({{\"{}\",{},}}),", chan->name, chan->host_mud, chan->status );
      I3_write_buffer( mud, s );
   }
   I3_write_buffer( mud, "]),})\r" );
}

void send_locate_req( i3_mud *mud, i3header *header, std::string_view tname )
{
   I3_write_header( mud, "locate-req", header->originator_mudname, header->originator_username, "", "" );
   I3_write_buffer( mud, "\"" );
   I3_write_buffer( mud, tname );
   I3_write_buffer( mud, "\",})\r" );
}

void process_locate_req( i3_mud *mud, i3header *header, char *s )
{
   char *ps = s, *next_ps;
   char tname[MIL];

   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( tname, ps, MIL );

   for( auto* tmud : mud_list )
   {
      if( tmud->locate )
         send_locate_req( tmud, header, tname );
   }
}

void send_ucache_update( i3_mud *mud, i3header *header, std::string_view username, std::string_view visname, int gender )
{
   std::string s;

   I3_write_header( mud, "ucache-update", header->originator_mudname, "", "", "" );
   I3_write_buffer( mud, "\"" );
   I3_write_buffer( mud, username );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, visname );
   I3_write_buffer( mud, "\"," );
   s = std::format( "{}", gender );
   I3_write_buffer( mud, s );
   I3_write_buffer( mud, ",})\r" );
}

void process_ucache_update( i3_mud *mud, i3header *header, char *s )
{
   char *ps = s, *next_ps;
   char username[MIL], visname[MIL];
   int gender;

   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( username, ps, MIL );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( visname, ps, MIL );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   gender = atoi( ps );

   for( auto* tmud : mud_list )
   {
      if( tmud->ucache )
         send_ucache_update( tmud, header, username, visname, gender );
   }
}

void process_locate_reply( i3_mud *tmud, i3header *header, char *s )
{
   char mud_name[MIL], user_name[MIL], status[MIL], s1[10];
   char *ps = s, *next_ps;
   int idletime;

   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strcpy( mud_name, ps );
   ps = next_ps;

   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strcpy( user_name, ps );
   ps = next_ps;

   I3_get_field( ps, &next_ps );
   idletime = atoi( ps );
   ps = next_ps;

   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strcpy( status, ps );
    
   I3_write_header( tmud, "locate-reply", header->originator_mudname, "", header->target_mudname, header->target_username );
   I3_write_buffer( tmud, "\"" );
   I3_write_buffer( tmud, mud_name );
   I3_write_buffer( tmud, "\",\"" );
   I3_write_buffer( tmud, user_name );
   if( tmud->version != 2 )
   {
      I3_write_buffer( tmud, "\"," );
      snprintf( s1, 10, "%d", idletime );
      I3_write_buffer( tmud, s1 );
      I3_write_buffer( tmud, ",\"" );
      I3_write_buffer( tmud, status );
   }
   I3_write_buffer( tmud, "\",})\r" );
}

void send_channel_ack( i3_mud *mud, i3header *header, std::string_view message )
{
   I3_write_header( mud, "chan-ack", I3_THISMUD, "", header->originator_mudname, header->originator_username );
   I3_write_buffer( mud, "\"" );
   I3_write_buffer( mud, message );
   I3_write_buffer( mud, "\",})\r" );
}

i3_channel *new_I3_channel( void )
{
   i3_channel *cnew = new i3_channel;
   channel_list.push_back( cnew );
   chan_count++;

   return cnew;
}

void destroy_I3_channel( i3_channel *channel )
{
   i3_mud *mud;

   if( channel == nullptr )
   {
      i3bug( "{}: Null parameter", __func__ );
      return;
   }

   i3log( "Channel {} being destroyed.", channel->name );

   mud = find_mud( channel->host_mud );
   i3_sendtoall( mud, "purgechannel", channel->name );

   deleteptr( channel );
   chan_count--;
}

void remove_chmudlist( i3_mud *mud, i3_channel *channel, i3header *header, std::string_view mudname )
{
   std::string buf;

   if( mudname.empty() )
      return;

   for( auto it = channel->m_listeners.begin(  ); it != channel->m_listeners.end(  ); )
   {
      mlist *m_list = *it;
      ++it;

      if( !str_cmp( m_list->name, mudname ) )
      {
         channel->m_listeners.remove( m_list );
         deleteptr( m_list );

         buf = std::format( "Mud {} removed from {} list for channel {}.",
            mudname, channel->status == 0 ? "ban" : "invite", channel->name );
         send_channel_ack( mud, header, buf );
         return;
      }
   }

   buf = std::format( "Mud {} is not {} channel {}.",
      mudname, channel->status == 0 ? "banned from" : "invited to", channel->name );
   send_channel_ack( mud, header, buf );
}

void add_chmudlist( i3_mud *mud, i3_channel *channel, i3header *header, std::string_view mudname )
{
   std::string buf;

   if( mudname.empty() )
      return;

   for( auto* m_list : channel->m_listeners )
   {
      if( !str_cmp( m_list->name, mudname ) )
      {
         buf = std::format( "Mud {} is already {} channel {}.",
            mudname, channel->status == 0 ? "banned from" : "invited to", channel->name );
         send_channel_ack( mud, header, buf );
         return;
      }
   }

   mlist *m_list = new mlist;
   m_list->name = mudname;
   channel->m_listeners.push_back( m_list );

   buf = std::format( "Mud {} added to {} list for channel {}.", mudname, channel->status == 0 ? "ban" : "invite", channel->name );
   send_channel_ack( mud, header, buf );
}

void process_channel_adminlist( i3_mud *mud, i3header *header, char *s )
{
   i3_channel *channel;
   char *ps = s, *next_ps;
   std::string buf;

   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   if( !( channel = find_channel( ps ) ) )
   {
      buf = std::format( "Channel {} does not exist on the router.", ps );
      I3_send_error( mud, header->originator_username, "unk-channel", buf, "" );
      return;
   }

   if( str_cmp( channel->host_mud, mud->name ) )
   {
      buf = std::format( "Cannot view administrative list for {}. You are not the host mud.", channel->name );
      I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
      return;
   }

   I3_write_header( mud, "chan-adminlist-reply", I3_THISMUD, "", header->originator_mudname, header->originator_username );
   I3_write_buffer( mud, "\"" );
   I3_write_buffer( mud, channel->name );
   I3_write_buffer( mud, "\",({" );
   for( auto* m_list : channel->m_listeners )
   {
      I3_write_buffer( mud, "\"" );
      I3_write_buffer( mud, m_list->name );
      I3_write_buffer( mud, "\"," );
   }
   I3_write_buffer( mud, "}),})\r" );
}

void process_channel_admin( i3_mud *mud, i3header *header, char *s )
{
   i3_channel *channel;
   char *ps = s, *next_ps;
   char chan[256];
   std::string buf;

   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( chan, ps, 256 );

   if( !( channel = find_channel( chan ) ) )
   {
      buf = std::format( "Channel {} does not exist on the router.", chan );
      I3_send_error( mud, header->originator_username, "unk-channel", buf, "" );
      return;
   }

   if( str_cmp( channel->host_mud, header->originator_mudname ) )
   {
      buf = std::format( "Unable to administer channel {}. You are not the host mud.", chan );
      I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
      return;
   }

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   ps += 2;

   while( 1 ) 
   {
      if( ps[0] == '}' )
         break;

      I3_get_field( ps, &next_ps );
      I3_remove_quotes( &ps );

      add_chmudlist( mud, channel, header, ps );
      save_channels();

      ps = next_ps;

      if( ps[0] == '}' )
         break;
   }

#ifndef PACKET_CHECK
   return;
#endif

   ps = next_ps;

   I3_get_field( ps, &next_ps );
   ps += 2;

   while( 1 ) 
   {
      if( ps[0] == '}' )
         break;

      I3_get_field( ps, &next_ps );
      I3_remove_quotes( &ps );

      remove_chmudlist( mud, channel, header, ps );
      save_channels();

      ps = next_ps;

      if( ps[0]== '}' )
         break;
   }
}

void process_channel_remove( i3_mud *mud, i3header *header, char *s )
{
   i3_channel *channel;
   char *ps = s, *next_ps;
   char chan[256];
   std::string buf;

   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( chan, ps, 256 );

   if( !( channel = find_channel( chan ) ) )
   {
      buf = std::format( "Unable to remove channel {}. No channel by that name exists.", chan );
      I3_send_error( mud, header->originator_username, "unk-channel", buf, "" );
      return;
   }

   if( str_cmp( channel->host_mud, header->originator_mudname ) )
   {
      buf = std::format( "Unable to remove channel {}. You are not the host mud.", chan );
      I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
      return;
   }

   destroy_I3_channel( channel );
   save_channels();

   buf = std::format( "Channel {} successfully removed from router.", chan );
   send_channel_ack( mud, header, buf );
}

void process_channel_add( i3_mud *mud, i3header *header, char *s )
{
   i3_channel *channel;
   char *ps = s, *next_ps;
   char chan[256];
   std::string buf;
   int status;

   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( chan, ps, 256 );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   status = atoi( ps );

   if( ( channel = find_channel( chan ) ) )
   {
      buf = std::format( "Unable to add channel {}. A channel by that name already exists.", chan );
      I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
      return;
   }

   if( status != 0 && status != 1 && status != 2 )
   {
      buf = std::format( "Unable to add channel {}. Status {} is not supported.", chan, status );
      I3_send_error( mud, header->originator_username, "bad-pkt", buf, "" );
      return;
   }

   channel = new_I3_channel();
   channel->name = chan;
   channel->host_mud = header->originator_mudname;
   channel->status = status;
   channel->purge_time = current_time + std::chrono::days( 30 );
   save_channels();

   i3_sendtoall( mud, "newchannel", channel->name );
   buf = std::format( "Channel {} successfully created on router.", channel->name );
   send_channel_ack( mud, header, buf );
   i3log( "Channel {} created by {}@{} with status {}",
      channel->name, header->originator_username, header->originator_mudname, channel->status );
}

void send_filter_req_t( i3_channel *channel, i3header *header, std::string_view tmud, std::string_view tuser, std::string_view msg_o, std::string_view msg_t, std::string_view tvis )
{
   i3_mud *mud;

   if( !( mud = find_mud( channel->host_mud ) ) )
   {
      i3bug( "Filtered channel request sent to non-existant host mud: {}", channel->host_mud );
      return;
   }

   I3_write_header( mud, "chan-filter-req", I3_THISMUD, "", channel->host_mud, "" );
   I3_write_buffer( mud, "\"" );
   I3_write_buffer( mud, channel->name );
   I3_write_buffer( mud, "\",({" );
   I3_write_buffer( mud, "\"channel-t\"," );
   I3_write_buffer( mud, "5,\"" );
   I3_write_buffer( mud, header->originator_mudname );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, header->originator_username );
   I3_write_buffer( mud, "\",0,0,\"" );
   I3_write_buffer( mud, channel->name );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, tmud );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, tuser );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, I3_escape( msg_o ) );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, I3_escape( msg_t ) );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, header->originator_username );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, tvis );
   I3_write_buffer( mud, "\",}),})\r" );
}

void send_filter_req( i3_channel *channel, i3header *header, bool emote, std::string_view visname, std::string_view message )
{
   i3_mud *mud;

   if( !( mud = find_mud( channel->host_mud ) ) )
   {
      i3bug( "Filtered channel request sent to non-existant host mud: {}", channel->host_mud );
      return;
   }

   I3_write_header( mud, "chan-filter-req", I3_THISMUD, "", channel->host_mud, "" );
   I3_write_buffer( mud, "\"" );
   I3_write_buffer( mud, channel->name );
   I3_write_buffer( mud, "\",({" );
   if( emote )
      I3_write_buffer( mud, "\"channel-e\"," );
   else
      I3_write_buffer( mud, "\"channel-m\"," );
   I3_write_buffer( mud, "5,\"" );
   I3_write_buffer( mud, header->originator_mudname );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, header->originator_username );
   I3_write_buffer( mud, "\",0,0,\"" );
   I3_write_buffer( mud, channel->name );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, visname );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, I3_escape( message ) );
   I3_write_buffer( mud, "\",}),})\r" );
}

void send_channel_t( i3_mud *mud, i3_channel *channel, i3header *header, std::string_view tmud, std::string_view tuser, std::string_view msg_o, std::string_view msg_t, std::string_view tvis )
{
   I3_write_header( mud, "channel-t", header->originator_mudname, header->originator_username, "", "" );
   I3_write_buffer( mud, "\"" );
   I3_write_buffer( mud, channel->name );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, tmud );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, tuser );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, I3_escape( msg_o ) );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, I3_escape( msg_t ) );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, header->originator_username );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, tvis );
   I3_write_buffer( mud, "\",})\r" );
}

void process_channel_t( i3_mud *mud, i3header *header, char *s, bool filterme )
{
   char *ps = s, *next_ps;
   char targetmud[MIL], targetuser[MIL], message_o[MSL], message_t[MSL];
   char visname_o[MIL], visname_t[MIL];
   std::string buf;
   std::list<i3_listener *>::iterator it;
   std::list<mlist *>::iterator it2;
   i3_channel *channel;
   i3_listener *listener;
   mlist *m_list;
   bool found2 = false;

   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );

   if( !( channel = find_channel( ps ) ) ) 
   {
      buf = std::format( "Channel {} is not known by {}", ps, I3_THISMUD );
      I3_send_error( mud, header->originator_username, "unk-channel", buf, "" );
      return;
   }

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( targetmud, ps, MIL );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( targetuser, ps, MIL );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( message_o, ps, MSL );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( message_t, ps, MSL );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( visname_o, ps, MIL );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( visname_t, ps, MIL );

   for( it2 = channel->m_listeners.begin(  ); it2 != channel->m_listeners.end(  ); ++it2 )
   {
      m_list = *it2;

      if( !str_cmp( mud->name, m_list->name ) && channel->status == 0 )
      {
         buf = std::format( "Your mud is not allowed on channel {}.", channel->name );
         I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
         return;
      }
   }

   for( it2 = channel->m_listeners.begin(  ); it2 != channel->m_listeners.end(  ); ++it2 )
   {
      m_list = *it2;

      if( !str_cmp( mud->name, m_list->name ) && ( channel->status == 1 || channel->status == 2 ) )
      {
         found2 = true;
         break;
      }
   }
   if( ( channel->status == 1 || channel->status == 2 ) && !found2 )
   {
      buf = std::format( "{} is a private channel your mud has not been invited to.", channel->name );
      I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
      return;
   }

   found2 = false;
   for( it = channel->listeners.begin(  ); it != channel->listeners.end(  ); ++it )
   {
      listener = *it;

      if( !str_cmp( mud->name, listener->name ) )
      {
         found2 = true;
         break;
      }
   }
   if( !found2 )
   {
      buf = std::format( "Your mud is not listening to {}. Cannot send message.", channel->name );
      I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
      return;
   }

   channel->purge_time = current_time + std::chrono::days( 30 );

   /* Filtered channel */
   if( channel->status == 2 && str_cmp( mud->name, channel->host_mud ) && filterme )
   {
      send_filter_req_t( channel, header, targetmud, targetuser, message_o, message_t, visname_t );
      return;
   }

   for( auto* tmud : mud_list )
   {
      for( it = channel->listeners.begin(  ); it != channel->listeners.end(  ); ++it )
      {
         listener = *it;

         if( !str_cmp( tmud->name, listener->name ) )
            send_channel_t( tmud, channel, header, targetmud, targetuser, message_o, message_t, visname_t );
      }
   }
}

void send_channel_e( i3_mud *mud, i3_channel *channel, i3header *header, std::string_view visname, std::string_view message )
{
   I3_write_header( mud, "channel-e", header->originator_mudname, header->originator_username, "", "" );
   I3_write_buffer( mud, "\"" );
   I3_write_buffer( mud, channel->name );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, visname );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, I3_escape( message ) );
   I3_write_buffer( mud, "\",})\r" );
}

void process_channel_e( i3_mud *mud, i3header *header, char *s, bool filterme )
{
   char *ps = s, *next_ps;
   char visname[MSL], message[MSL];
   std::string buf;
   std::list<i3_listener *>::iterator it;
   std::list<mlist *>::iterator it2;
   i3_channel *channel;
   i3_listener *listener;
   mlist *m_list;
   bool found2 = false;

   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );

   if( !( channel = find_channel( ps ) ) ) 
   {
      buf = std::format( "Channel {} is not known by {}", ps, I3_THISMUD );
      I3_send_error( mud, header->originator_username, "unk-channel", buf, "" );
      return;
   }

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( visname, ps, MSL );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( message, ps, MSL );

   for( it2 = channel->m_listeners.begin(  ); it2 != channel->m_listeners.end(  ); ++it2 )
   {
      m_list = *it2;

      if( !str_cmp( mud->name, m_list->name ) && channel->status == 0 )
      {
         buf = std::format( "Your mud is not allowed on channel {}.", channel->name );
         I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
         return;
      }
   }

   for( it2 = channel->m_listeners.begin(  ); it2 != channel->m_listeners.end(  ); ++it2 )
   {
      m_list = *it2;

      if( !str_cmp( mud->name, m_list->name ) && ( channel->status == 1 || channel->status == 2 ) )
      {
         found2 = true;
         break;
      }
   }

   if( ( channel->status == 1 || channel->status == 2 ) && !found2 )
   {
      buf = std::format( "{} is a private channel your mud has not been invited to.", channel->name );
      I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
      return;
   }

   found2 = false;
   for( it = channel->listeners.begin(  ); it != channel->listeners.end(  ); ++it )
   {
      listener = *it;

      if( !str_cmp( mud->name, listener->name ) )
      {
         found2 = true;
         break;
      }
   }
   if( !found2 )
   {
      buf = std::format( "Your mud is not listening to {}. Cannot send message.", channel->name );
      I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
      return;
   }

   channel->purge_time = current_time + std::chrono::days( 30 );

   /* Filtered channel */
   if( channel->status == 2 && str_cmp( mud->name, channel->host_mud ) && filterme )
   {
      send_filter_req( channel, header, true, visname, message );
      return;
   }

   for( auto* tmud : mud_list )
   {
      for( it = channel->listeners.begin(  ); it != channel->listeners.end(  ); ++it )
      {
         listener = *it;

         if( !str_cmp( tmud->name, listener->name ) )
            send_channel_e( tmud, channel, header, visname, message );
      }
   }
}

void send_channel_m( i3_mud *mud, i3_channel *channel, i3header *header, std::string_view visname, std::string_view message )
{
   I3_write_header( mud, "channel-m", header->originator_mudname, header->originator_username, "", "" );
   I3_write_buffer( mud, "\"" );
   I3_write_buffer( mud, channel->name );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, visname );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, I3_escape( message ) );
   I3_write_buffer( mud, "\",})\r" );
}

void process_channel_m( i3_mud *mud, i3header *header, char *s, bool filterme )
{
   char *ps = s, *next_ps;
   char visname[MSL], message[MSL];
   std::string buf;
   i3_channel *channel;
   i3_listener *listener;
   mlist *m_list;
   std::list<i3_listener *>::iterator it;
   std::list<mlist *>::iterator it2;
   bool found2 = false;

   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );

   if( !( channel = find_channel( ps ) ) )
   {
      buf = std::format( "Channel {} is not known by {}", ps, I3_THISMUD );
      I3_send_error( mud, header->originator_username, "unk-channel", buf, "" );
      return;
   }

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( visname, ps, MSL );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( message, ps, MSL );

   for( it2 = channel->m_listeners.begin(  ); it2 != channel->m_listeners.end(  ); ++it2 )
   {
      m_list = *it2;

      if( !str_cmp( mud->name, m_list->name ) && channel->status == 0 )
      {
         buf = std::format( "Your mud is not allowed on channel {}.", channel->name );
         I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
         return;
      }
   }

   for( it2 = channel->m_listeners.begin(  ); it2 != channel->m_listeners.end(  ); ++it2 )
   {
      m_list = *it2;

      if( !str_cmp( mud->name, m_list->name ) && ( channel->status == 1 || channel->status == 2 ) )
      {
         found2 = true;
         break;
      }
   }

   if( ( channel->status == 1 || channel->status == 2 ) && !found2 )
   {
      buf = std::format( "{} is a private channel your mud has not been invited to.", channel->name );
      I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
      return;
   }

   found2 = false;
   for( it = channel->listeners.begin(  ); it != channel->listeners.end(  ); ++it )
   {
      listener = *it;

      if( !str_cmp( mud->name, listener->name ) )
      {
         found2 = true;
         break;
      }
   }
   if( !found2 )
   {
      buf = std::format( "Your mud is not listening to {}. Cannot send message.", channel->name );
      I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
      return;
   }

   channel->purge_time = current_time + std::chrono::days( 30 );

   /* Filtered channel */
   if( channel->status == 2 && str_cmp( mud->name, channel->host_mud ) && filterme )
   {
      send_filter_req( channel, header, false, visname, message );
      return;
   }

   for( auto* tmud : mud_list )
   {
      for( it = channel->listeners.begin(  ); it != channel->listeners.end(  ); ++it )
      {
         listener = *it;

         if( !str_cmp( tmud->name, listener->name ) )
            send_channel_m( tmud, channel, header, visname, message );
      }
   }
}

void process_channel_filter( i3_mud *mud, i3header *header, char *s )
{
   char *ps = s, *next_ps;
   char ptype[MIL];
   std::string buf;
   i3_channel *channel;
   i3header *second_header;

   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );

   if( !( channel = find_channel( ps ) ) )
   {
      buf = std::format( "Filtered channel {} is not known by {}", ps, I3_THISMUD );
      I3_send_error( mud, header->originator_username, "unk-channel", buf, "" );
      return;
   }

   if( str_cmp( channel->host_mud, mud->name ) )
   {
      buf = std::format( "Cannot accept filter-reply for {}. You are not the host mud.", channel->name );
      I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
      return;
   }

   ps = next_ps;

   if( ps[0] != '(' || ps[1] != '{' )
      return;

   ps += 2;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( ptype, ps, MIL );

   second_header = I3_get_header( &ps );

   if( !str_cmp( ptype, "channel-m" ) )
      process_channel_m( mud, second_header, ps, false );
   else if( !str_cmp( ptype, "channel-e" ) )
      process_channel_e( mud, second_header, ps, false );
   else if( !str_cmp( ptype, "channel-t" ) )
      process_channel_t( mud, second_header, ps, false );
   else
   {
      buf = std::format( "Invalid channel packet: {}. Cannot filter the request.", ptype );
      I3_send_error( mud, header->originator_username, "unk-type", buf, "" );
   }
   deleteptr( second_header );
}

void process_channel_listen( i3_mud *mud, i3header *header, char *s )
{
   i3_channel *channel;
   mlist *m_list;
   std::list<mlist *>::iterator it;
   char *ps = s, *next_ps;
   char chan[256];
   std::string buf, ebuf;
   int status;
   bool found2 = false;

   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( chan, ps, 256 );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   status = std::stoi( ps );

   if( !( channel = find_channel( chan ) ) )
   {
      buf = std::format( "Channel {} is not known by {}", chan, I3_THISMUD );
      I3_send_error( mud, header->originator_username, "unk-channel", buf, "" );
      return;
   }

   for( it = channel->m_listeners.begin(  ); it != channel->m_listeners.end(  ); ++it )
   {
      m_list = *it;

      if( !str_cmp( mud->name, m_list->name ) && channel->status == 0 )
      {
         buf = std::format( "Your mud is not allowed on channel {}.", channel->name );
         ebuf = std::format( "({{\"channel-listen\",5,\"{}\",0,\"{}\",0,\"{}\",0,}})",
            header->originator_mudname, I3_THISMUD, channel->name );
         I3_send_error( mud, header->originator_username, "not-allowed", buf, ebuf );
         return;
      }
   }

   for( it = channel->m_listeners.begin(  ); it != channel->m_listeners.end(  ); ++it )
   {
      m_list = *it;

      if( !str_cmp( mud->name, m_list->name ) && ( channel->status == 1 || channel->status == 2 ) )
      {
         found2 = true;
         break;
      }
   }

   if( ( channel->status == 1 || channel->status == 2 ) && !found2 )
   {
      buf = std::format( "{} is a private channel your mud has not been invited to.", channel->name );
      ebuf = std::format( "({{\"channel-listen\",5,\"{}\",0,\"{}\",0,\"{}\",1,}})",
         header->originator_mudname, I3_THISMUD, channel->name );
      I3_send_error( mud, header->originator_username, "not-allowed", buf, ebuf );
      return;
   }

   /* Asking to unsubscribe */
   if( status == 0 )
   {
      for( auto it2 = channel->listeners.begin(  ); it2 != channel->listeners.end(  ); )
      {
         i3_listener *listener = *it2;
         ++it2;

         if( !str_cmp( mud->name, listener->name ) )
         {
            channel->listeners.remove( listener );
            deleteptr( listener );
            return;
         }
      }
      return;
   }

   if( status == 1 )
   {
      channel->purge_time = current_time + std::chrono::days( 30 );
      for( auto* listener : channel->listeners )
      {
         if( !str_cmp( mud->name, listener->name ) )
         {
            buf = std::format( "{} is already subscribed to %s", mud->name, channel->name );
            I3_send_error( mud, header->originator_username, "not-allowed", buf, "" );
            return;
         }
      }
      i3_listener *listener = new i3_listener;
      listener->name = mud->name;
      channel->listeners.push_back( listener );
      return;
   }
   buf = std::format( "Invalid status code: {}. Received from channel-listen.", status );
   I3_send_error( mud, header->originator_username, "bad-pkt", buf, "" );
}

void send_one_mud( i3_mud *mud, i3_mud *list )
{
   std::string s;

   I3_write_buffer( mud, "\"" );
   I3_write_buffer( mud, list->name );
   I3_write_buffer( mud, "\":({" );
   s = std::format( "{},\"", list->status );
   I3_write_buffer( mud, s );
   I3_write_buffer( mud, list->ipaddress );
   s = std::format( "\",{},{},{},\"", list->player_port, list->imud_tcp_port, list->imud_udp_port );
   I3_write_buffer( mud, s );
   I3_write_buffer( mud, list->mudlib );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, list->base_mudlib );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, list->driver );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, list->mud_type );
   I3_write_buffer( mud, "\",\"" );
   I3_write_buffer( mud, list->open_status );
   if( mud->version > 1 )
   {
      I3_write_buffer( mud, "\",\"" );
      I3_write_buffer( mud, list->admin_email );
   }
   I3_write_buffer( mud, "\"," );

   /* First Mapping */
   I3_write_buffer( mud, "([" );
   if( list->emoteto )
      I3_write_buffer( mud, "\"emoteto\":1," );
   if( list->news )
      I3_write_buffer( mud, "\"news\":1," );
   if( list->ucache )
      I3_write_buffer( mud, "\"ucache\":1," );
   if( list->auth )
      I3_write_buffer( mud, "\"auth\":1," );
   if( list->locate )
      I3_write_buffer( mud, "\"locate\":1," );
   if( list->finger )
      I3_write_buffer( mud, "\"finger\":1," );
   if( list->channel )
      I3_write_buffer( mud, "\"channel\":1," );
   if( list->who )
      I3_write_buffer( mud, "\"who\":1," );
   if( list->tell )
      I3_write_buffer( mud, "\"tell\":1," );
   if( list->beep )
      I3_write_buffer( mud, "\"beep\":1," );
   if( list->mail )
      I3_write_buffer( mud, "\"mail\":1," );
   if( list->file )
      I3_write_buffer( mud, "\"file\":1," );
   if( list->http )
   {
      s = std::format( "\"http\":{},", list->http );
      I3_write_buffer( mud, s );
   }
   if( list->smtp )
   {
      s = std::format( "\"smtp\":{},", list->smtp );
      I3_write_buffer( mud, s );
   }
   if( list->pop3 )
   {
      s = std::format( "\"pop3\":{},", list->pop3 );
      I3_write_buffer( mud, s );
   }
   if( list->ftp )
   {
      s = std::format( "\"ftp\":{},", list->ftp );
      I3_write_buffer( mud, s );
   }
   if( list->nntp )
   {
      s = std::format( "\"nntp\":{},", list->nntp );
      I3_write_buffer( mud, s );
   }
   if( list->rcp )
   {
      s = std::format( "\"rcp\":{},", list->rcp );
      I3_write_buffer( mud, s );
   }
   if( list->amrcp )
   {
      s = std::format( "\"amrcp\":{},", list->amrcp );
      I3_write_buffer( mud, s );
   }
   I3_write_buffer( mud, "])," );

   if( ( !list->banner.empty() || !list->daemon.empty() || !list->web.empty() || !list->time.empty() || list->jeamland ) && mud->version > 1 )
   {
      I3_write_buffer( mud, "([" );
   
      /* END First Mapping, Start Second Mapping */
      if( !list->banner.empty() )
      {
         s = std::format( "\"banner\":\"{}\",", list->banner );
         I3_write_buffer( mud, s );
      }
      if( !list->daemon.empty() )
      {
         s = std::format( "\"daemon\":\"{}\",", list->daemon );
         I3_write_buffer( mud, s );
      }
      if( !list->web.empty() )
      {
         s = std::format( "\"url\":\"{}\",", list->web );
         I3_write_buffer( mud, s );
      }
      if( !list->time.empty() )
      {
         s = std::format( "\"time\":\"{}\",", list->time );
         I3_write_buffer( mud, s );
      }
      if( list->jeamland )
      {
         s = std::format( "\"is_jeamland\":{},", list->jeamland );
         I3_write_buffer( mud, s );
      }
      I3_write_buffer( mud, "])" );
   }
   else
      I3_write_buffer( mud, "0" );
   I3_write_buffer( mud, ",})," );
}

void send_mudlist( i3_mud *mud )
{
   int i, lcount = 0;

   I3_write_header( mud, "mudlist", I3_THISMUD, "", mud->name, "" );
   mud->mudlist_id++;

   std::string s = std::format( "{},([", mud->mudlist_id );
   I3_write_buffer( mud, s );

   for( i = mud->mcount; i < MAX_CONN; i++ )
   {
      lcount++;
      if( mlist_array[i] == nullptr )
      {
         mud->mcount = -1;
         break;
      }
      if( lcount == 10 )
         break;
      mud->mcount++;
      send_one_mud( mud, mlist_array[i] );
   }
   I3_write_buffer( mud, "]),})\r" );
}

void ev_sendmudlist( void *data )
{
   i3_mud *mud;

   mud = (i3_mud *)data;
   mud->mcount = 0;

   send_mudlist( mud );
}

/* One time thing */
void ev_sendchanlist( void *data )
{
   i3_mud *mud;

   mud = (i3_mud *)data;

   send_chanlist( mud );
}

void send_startup_reply( i3_mud *mud )
{
   std::string s;

   I3_write_header( mud, "startup-reply", I3_THISMUD, "", mud->name, "" );
   I3_write_buffer( mud, "({({\"" );
   I3_write_buffer( mud, I3_THISMUD );
   s = std::format( "\",\"{} {}", I3_THISIP, this_mud->player_port );
   I3_write_buffer( mud, s );
   I3_write_buffer( mud, "\",}),})," );
   s = std::format( "{},}})\r", mud->password );
   I3_write_buffer( mud, s );
   add_event( 2, ev_sendchanlist, mud );
   add_event( 3, ev_sendmudlist, mud );
}

void process_shutdown( i3_mud *mud, i3header *header, char *s )
{
   char *ps = s, *next_ps;
   int shutdown_delay = 0;

   if( !mud )
   {
      i3bug( "{}: nullptr mud!", __func__ );
      return;
   }
   I3_get_field( ps, &next_ps );
   shutdown_delay = std::stoi( ps );

   mud->status = shutdown_delay;

   i3log( "Shutdown received from {} with delay {}.", mud->name, shutdown_delay );
   close_socket( mud->desc );
}

void destroy_I3_mud( i3_mud *mud )
{
   if( mud == nullptr )
   {
      i3bug( "{}: Null parameter", __func__ );
      return;
   }

   i3log( "Mud {} being removed from list.", mud->name );
   for( int i = 0; i < MAX_CONN; i++ )
   {
      if( mlist_array[i] == mud )
         mlist_array[i] = nullptr;
   }

   i3_sendtoall( nullptr, "purgeentry", mud->name );
   deleteptr( mud );
   mud_count--;
}

i3_mud *new_I3_mud( std::string_view name )
{
   i3_mud *cnew = new i3_mud;

   cnew->name = name;
   cnew->purge_time = current_time + std::chrono::days( 15 );
   mud_list.push_back( cnew );

   return cnew;
}

void save_mudlist( void )
{
   std::ofstream stream( std::filesystem::path{MUD_FILE} );
   if( !stream.is_open() )
      i3bug( "{}: Cannot open {} for writing: {}", __func__, MUD_FILE, std::strerror(errno) );
   else
   {
      i3log( "Saving mudlist..." );
      for( auto* mud : mud_list )
      {
         stream << "#MUDLIST\n";
         stream << std::format( "Name         {}~\n", mud->name );
         stream << std::format( "Version      {}\n",  mud->version );
         if( mud->desc != nullptr )
         {
            std::chrono::system_clock::time_point purge_time = current_time + std::chrono::days( 15 );
            auto purgetime = std::chrono::system_clock::to_time_t( purge_time );

            stream << std::format( "Purgetime    {}\n", purgetime );
         }
         else
         {
            auto mud_purge_time = std::chrono::system_clock::to_time_t( mud->purge_time );
            stream << std::format( "Purgetime    {}\n", mud_purge_time );
         }
         stream << std::format( "Status       {}\n",  mud->status );
         stream << std::format( "IP           {}~\n", mud->ipaddress );
         stream << std::format( "Mudlib       {}~\n", mud->mudlib );
         stream << std::format( "Baselib      {}~\n", mud->base_mudlib );
         stream << std::format( "Driver       {}~\n", mud->driver );
         stream << std::format( "Type         {}~\n", mud->mud_type );
         stream << std::format( "Openstatus   {}~\n", mud->open_status );
         stream << std::format( "Email        {}~\n", mud->admin_email );
         if( !mud->telnet.empty() )
            stream << std::format( "Telnet       {}~\n", mud->telnet );
         stream << std::format( "Ports        {} {} {}\n", mud->player_port, mud->imud_tcp_port, mud->imud_udp_port );
         stream << std::format( "Password     {} {} {}\n", mud->password, mud->mudlist_id, mud->chanlist_id );
         stream << std::format( "Services     {} {} {} {} {} {} {} {} {} {} {} {}\n",
            mud->tell, mud->beep, mud->emoteto, mud->who, mud->finger, mud->locate, mud->channel, mud->news, mud->mail,
            mud->file, mud->auth, mud->ucache );
         stream << std::format( "OOBports     {} {} {} {} {} {} {}\n", mud->smtp, mud->ftp, mud->nntp, mud->http, mud->pop3,
            mud->rcp, mud->amrcp );
         if( !mud->banner.empty() )
            stream << std::format( "Banner       {}~\n", mud->banner );
         if( !mud->web.empty() )
            stream << std::format( "Web          {}~\n", mud->web );
         if( !mud->time.empty() )
            stream << std::format( "Time         {}~\n", mud->time );
         if( !mud->daemon.empty() )
            stream << std::format( "Daemon       {}~\n", mud->daemon );
         if( mud->jeamland )
            stream << std::format( "Jeamland     {}\n", mud->jeamland );
         stream << "End\n\n";
      }
      stream << "#END\n";
      stream.close();
      if( stream.fail() )
         i3bug( "{}: Error occurred after closing {}: ", __func__, MUD_FILE, std::strerror(errno) );
      else
         i3log( "Mudlist saved." );
   }
}

void fread_mudlist( std::ifstream & stream, i3_mud *mud )
{
   std::string key;
   while( stream >> key )
   {
      if( key == "Version" )
         stream >> mud->version;
      else if( key == "Purgetime" )
      {
         time_t loaded_time;
         stream >> loaded_time;

         mud->purge_time = std::chrono::system_clock::from_time_t( loaded_time );
      }
      else if( key == "Status" )
         stream >> mud->status;
      else if( key == "IP" )
         mud->ipaddress = i3fread_line( stream, '~' );
      else if( key == "Mudlib" )
         mud->mudlib = i3fread_line( stream, '~' );
      else if( key == "Baselib" )
         mud->base_mudlib = i3fread_line( stream, '~' );
      else if( key == "Driver" )
         mud->driver = i3fread_line( stream, '~' );
      else if( key == "Type" )
         mud->mud_type = i3fread_line( stream, '~' );
      else if( key == "Openstatus" )
         mud->open_status = i3fread_line( stream, '~' );
      else if( key == "Email" )
         mud->admin_email = i3fread_line( stream, '~' );
      else if( key == "Telnet" )
         mud->telnet = i3fread_line( stream, '~' );
      else if( key == "Ports" )
         stream >> mud->player_port >> mud->imud_tcp_port >> mud->imud_udp_port;
      else if( key == "Password" )
         stream >> mud->password >> mud->mudlist_id >> mud->chanlist_id;
      else if( key == "Services" )
         stream >> mud->tell >> mud->beep >> mud->emoteto >> mud->who >> mud->finger >> mud->locate >> mud->channel >> mud->news >> mud->mail >> mud->file >> mud->auth >> mud->ucache;
      else if( key == "OOBports" )
         stream >> mud->smtp >> mud->ftp >> mud->nntp >> mud->http >> mud->pop3 >> mud->rcp >> mud->amrcp;
      else if( key == "Banner" )
         mud->banner = i3fread_line( stream, '~' );
      else if( key == "Web" )
         mud->web = i3fread_line( stream, '~' );
      else if( key == "Time" )
         mud->time = i3fread_line( stream, '~' );
      else if( key == "Daemon" )
         mud->daemon = i3fread_line( stream, '~' );
      else if( key == "Jeamland" )
         stream >> mud->jeamland;
      else if( key == "End" )
         return;
      else
      {
         i3bug( "{}: Bad section '{}' in mudlist file - skipping.", __func__, key );
         i3fread_to_eol( stream );
      }
   }
}

void I3_loadmudlist( void )
{
   mud_count = 0;
   i3log( "Loading mudlist..." );

   std::ifstream stream( std::filesystem::path{MUD_FILE} );
   if( !stream.is_open() )
   {
      i3bug( "{}: Cannot open {} for reading: {}", __func__, MUD_FILE, std::strerror(errno) );
      return;
   }

   std::string key;
   while( stream >> key )
   {
      if( key == "#MUDLIST" )
      {
         std::string word = i3fread_word( stream );
         if( !str_cmp( word, "Name" ) )
         {
            std::string tmpname = i3fread_line( stream, '~' );

            i3_mud *mud = new_I3_mud( tmpname );
            fread_mudlist( stream, mud );
            if( mud_count >= MAX_CONN )
            {
               i3log( "FATAL: Mud count exceeds capacity of array. PLEASE ADJUST! Mud {} will not be added to the list.", mud->name );
               deleteptr( mud );
            }
            else
            {
               mlist_array[mud_count] = mud;
               mud_count++;
               mud->status = 0; /* No muds could possibly be "up" if we just rebooted */
            }
         }
         else
         {
            i3bug( "{}: No mudname saved, skipping entry.", __func__ );
            i3fread_to_eol( stream );
         }
         continue;
      }
      else if( key == "#END" )
         break;
      else
      {
         i3bug( "{}: bad section: {}.", __func__, key );
         continue;
      }
   }
   stream.close();
   i3log( "Mudlist loaded. {} muds found.", mud_count );
}

std::string strip_noprint( std::string_view src )
{
   std::string result;
   result.reserve( src.size() );

   for( unsigned char c : src )
   {
      if( std::isprint(c) )
         result.push_back( static_cast<char>(c) );
      else
         result.push_back( '*' );
   }
   return result;
}

void process_startup_req( descriptor_data *d, i3header *header, std::string_view ptype, char *s )
{
   i3_mud *mud = nullptr;
   char *ps = s, *next_ps;
   int password = -1, i;
   bool newmud = false;

   mud = find_mud( header->originator_mudname );

   if( !mud )
   {
      mud = new_I3_mud( header->originator_mudname );
      mud->desc = d;
      mud->mcount = -1;
      d->mud = mud;
      i3log( "Adding new mud: {}. Processing startup.", mud->name );
      mud_count++;
      for( i = 0; i < MAX_CONN; i++ )
      {
         if( mlist_array[i] == nullptr )
         {
            mlist_array[i] = mud;
            break;
         }
      }
      if( i >= MAX_CONN )
         i3log( "FATAL: Mudlist array has exceeded capacity. PLEASE ADJUST. Mud {} has not been added to the list.", mud->name );
      newmud = true;
   }
   else
   {
      i3log( "Existing mud: {}. Processing startup.", mud->name );
      if( mud->desc != nullptr )
      {

         // long newsize;

         /*
          * This has to be a custom created error packet - we cannot use the mud's link to assemble one.
          * It also needs to be manually buffered here and sent immediately. The connection is not valid yet.
          */
         i3log( "Attempted duplicate connection for {}. Dropping link.", header->originator_mudname );
         std::string ebuf = std::format( "({{\"error\",5,\"{}\",0,\"{}\",0,\"unk-src\",\"{} is already connected.\",0,}})\r",
            I3_THISMUD, header->originator_mudname, header->originator_mudname );

         d->output_buffer.append( ebuf );
         I3_send_packet( d );
         close_socket( d );
         return;
      }
      mud->mcount = -1;
      mud->desc = d;
      d->mud = mud;
   }
   mud->status = -1;

   if( !str_cmp( ptype, "startup-req-1" ) )
      mud->version = 1;
   else if( !str_cmp( ptype, "startup-req-2" ) )
      mud->version = 2;
   else
      mud->version = 3;

   I3_get_field( ps, &next_ps );
   password = std::stoi( ps );

   if( !newmud && password != 0 && password != mud->password && str_cmp( d->host, mud->ipaddress ) )
   {
      i3log( "Password for {} does not match! Dropping connection.", mud->name );
      close_socket( d );
      return;
   }

   mud->password = random_number( 20000, 2000000000 );
   mud->ipaddress = d->host;

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   mud->mudlist_id = atoi( ps );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   mud->chanlist_id = atoi( ps );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   mud->player_port = atoi( ps );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   mud->imud_tcp_port = atoi( ps );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   mud->imud_udp_port = atoi( ps );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   mud->mudlib = strip_noprint( ps );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   mud->base_mudlib = strip_noprint( ps );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   mud->driver = strip_noprint( ps );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   mud->mud_type = strip_noprint( ps );

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   mud->open_status = strip_noprint( ps );

   if( str_cmp( ptype, "startup-req-1" ) )
   {
      ps = next_ps;
      I3_get_field( ps, &next_ps );
      I3_remove_quotes( &ps );
      mud->admin_email = strip_noprint( ps );
   }

   ps = next_ps;
   I3_get_field( ps, &next_ps );
   ps += 2;

   while( 1 )
   {
      char *next_ps3;
      char key[MIL];

      if( ps[0] == ']' )
         break;

      I3_get_field( ps, &next_ps3 );
      I3_remove_quotes( &ps );
      strlcpy( key, ps, MIL );
      ps = next_ps3;
      I3_get_field( ps, &next_ps3 );

      switch( key[0] )
      {
         case 'a':
            if( !str_cmp( key, "auth" ) )
            {
               mud->auth = ps[0] == '0' ? 0 : 1;
               break;
            }
            if( !str_cmp( key, "amrcp" ) )
            {
               mud->amrcp = atoi( ps );
               break;
            }
            break;
         case 'b':
            if( !str_cmp( key, "beep" ) )
            {
               mud->beep = ps[0] == '0' ? 0 : 1;
               break;
            }
            break;
         case 'c':
            if( !str_cmp( key, "channel" ) )
            {
               mud->channel = ps[0] == '0' ? 0 : 1;
               break;
            }
            break;
         case 'e':
            if( !str_cmp( key, "emoteto" ) )
            {
               mud->emoteto = ps[0] == '0' ? 0 : 1;
               break;
            }
            break;
         case 'f':
            if( !str_cmp( key, "file" ) )
            {
               mud->file = ps[0] == '0' ? 0 : 1;
               break;
            }
            if( !str_cmp( key, "finger" ) )
            {
               mud->finger = ps[0] == '0' ? 0 : 1;
               break;
            }
            if( !str_cmp( key, "ftp" ) )
            {
               mud->ftp = atoi( ps );
               break;
            }
            break;
         case 'h':
            if( !str_cmp( key, "http" ) )
            {
               mud->http = atoi( ps );
               break;
            }
            break;
         case 'l':
            if( !str_cmp( key, "locate" ) )
            {
               mud->locate = ps[0] == '0' ? 0 : 1;
               break;
            }
            break;
         case 'm':
            if( !str_cmp( key, "mail" ) )
            {
               mud->mail = ps[0] == '0' ? 0 : 1;
               break;
            }
            break;
         case 'n':
            if( !str_cmp( key, "news" ) )
            {
               mud->news = ps[0] == '0' ? 0 : 1;
               break;
            }
            if( !str_cmp( key, "nntp" ) )
            {
               mud->nntp = atoi( ps );
               break;
            }
            break;
         case 'p':
            if( !str_cmp( key, "pop3" ) )
            {
               mud->pop3 = atoi( ps );
               break;
            }
            break;
         case 'r':
            if( !str_cmp( key, "rcp" ) )
            {
               mud->rcp = atoi( ps );
               break;
            }
            break;
         case 's':
            if( !str_cmp( key, "smtp" ) )
            {
               mud->smtp = atoi( ps );
               break;
            }
            break;
         case 't':
            if( !str_cmp( key, "tell" ) )
            {
               mud->tell = ps[0] == '0' ? 0 : 1;
               break;
            }
            break;
            case 'u':
            if( !str_cmp( key, "ucache" ) )
            {
               mud->ucache = ps[0] == '0' ? 0 : 1;
               break;
            }
            break;
         case 'w':
            if( !str_cmp( key, "who" ) )
            {
               mud->who = ps[0] == '0' ? 0 : 1;
               break;
            }
            break;
         default:
            break;
      }
      ps = next_ps3;
      if( ps[0] == ']' )
         break;
   }
   ps = next_ps;
   I3_get_field( ps, &next_ps );

   if( ps[0] != '0' )
   {
      ps += 2;

      while( 1 )
      {
         char *next_ps4;
         char key[MIL];

         if( ps[0] == ']' )
            break;

         I3_get_field( ps, &next_ps4 );
         I3_remove_quotes( &ps );
         strlcpy( key, ps, MIL );
         ps = next_ps4;
         I3_get_field( ps, &next_ps4 );

         /* As more types become known, they'll end up here. Unknowns will be logged for future handling */
         switch( key[0] )
         {
            case 'b':
               if( !str_cmp( key, "banner" ) )
               {
                  I3_remove_quotes( &ps );
                  mud->banner = ps;
                  break;
               }
               break;

            case 'd':
               if( !str_cmp( key, "daemon" ) )
               {
                  I3_remove_quotes( &ps );
                  mud->daemon = strip_noprint( ps );
                  break;
               }
               break;

            case 'i':
               if( !str_cmp( key, "is_jeamland" ) )
               {
                  mud->jeamland = atoi( ps );
                  break;
               }
               break;

            case 't':
               if( !str_cmp( key, "time" ) )
               {
                  I3_remove_quotes( &ps );
                   mud->time = strip_noprint( ps );
                  break;
               }
               break;

            case 'u':
               if( !str_cmp( key, "url" ) )
               {
                  I3_remove_quotes( &ps );
                  mud->web = strip_noprint( ps );
                  break;
               }
               break;

            default:
               i3log( "Unknown key: '{}' with value '{}' in mudlist data", key, ps );
               break;
         }
         ps = next_ps4;
         if( ps[0] == ']' )
            break;
      }
   }
   i3log( "Startup complete for {}. Sending startup_reply.", mud->name );
   send_startup_reply( mud );
   save_mudlist();
}

void i3_sendtoall( i3_mud *mud, std::string_view type, std::string_view msg )
{
   std::string s;

   if( !str_cmp( type, "mudlist" ) )
   {
      for( auto* tmud : mud_list )
      {
         if( !tmud->desc )
            continue;

         I3_write_header( tmud, "mudlist", I3_THISMUD, "", tmud->name, "" );
         tmud->mudlist_id++;
         s = std::format( "{},([", tmud->mudlist_id );
         I3_write_buffer( tmud, s );
         send_one_mud( tmud, mud );
         I3_write_buffer( tmud, "]),})\r" );
      }
      return;
   }

   if( !str_cmp( type, "purgeentry" ) )
   {
      for( auto* tmud : mud_list )
      {
         if( mud == tmud )
            continue;

         if( !tmud->desc )
            continue;

         I3_write_header( tmud, "mudlist", I3_THISMUD, "", tmud->name, "" );
         tmud->mudlist_id++;
         s = std::format( "{},([\"{}\":0,]),}})\r", tmud->mudlist_id, msg );
         I3_write_buffer( tmud, s );
      }
      return;
   }

   if( !str_cmp( type, "newchannel" ) )
   {
      i3_channel *channel = find_channel( msg );

      for( auto* tmud : mud_list )
      {
         if( !tmud->desc )
            continue;

         I3_write_header( tmud, "chanlist-reply", I3_THISMUD, "", tmud->name, "" );
         tmud->chanlist_id++;
         s = std::format( "{},([\"{}\":({{\"{}\",{},}}),]),}})\r",
            tmud->chanlist_id, channel->name, channel->host_mud, channel->status );
         I3_write_buffer( tmud, s );
      }
      return;
   }

   if( !str_cmp( type, "purgechannel" ) )
   {
      for( auto* tmud : mud_list )
      {
         if( !tmud->desc )
            continue;

         I3_write_header( tmud, "chanlist-reply", I3_THISMUD, "", tmud->name, "" );
         tmud->chanlist_id++;
         s = std::format( "{},([\"{}\":0,]),}})\r", tmud->chanlist_id, msg );
         I3_write_buffer( tmud, s );
      }
      return;
   }

   if( !str_cmp( type, "router_shutdown" ) )
   {
      int delay = 5;

      if( !str_cmp( msg, "kill" ) )
         delay = 0;

      for( auto* tmud : mud_list )
      {
         if( !tmud->desc )
            continue;

         I3_write_header( tmud, "router-shutdown", I3_THISMUD, "", tmud->name, "" );
         s = std::format( "{},}})\r", delay );
         I3_write_buffer( tmud, s );
      }
      return;
   }
   i3log( "Uh... unknown type {} with message {} sent to i3_sendtoall!", type, msg );
}

/*
 * Read one I3 packet into the I3_input_buffer for a descriptor.
 * Chop off the final byte as a nullptr according to the size data received.
 * Also nullptr's if it finds a \r which is usually sent by Diku clients new and old.
 * Not totally foolproof, but should do for now.
 */
// The above logic was missing from the function. It has been added now. - Samson 7/4/2026 [Happy 250th USA!]
void I3_read_packet( descriptor_data *d )
{
   // We need at least 4 bytes to even know how big the packet is.
   if( d->I3_input_pointer < 4 )
      return;

   // Copy the length, then convert.
   uint32_t size;
   memcpy( &size, d->I3_input_buffer, 4 );
   size = ntohl( size );

   // Validate that the incoming size is valid. If not, kick the descriptor.
   if( size == 0 || size > IPS )
   {
      i3log( "CRITICAL: Received invalid packet size: {}", size );
      i3log( "Closing link to {}", ( d->mud && !d->mud->name.empty() ? d->mud->name : d->host ) );
      close_socket( d );
      return;
   }

   // Is the entire packet here yet?
   if( d->I3_input_pointer < (size + 4) )
      return;

   // Extract it into the current packet buffer.
   memcpy( d->I3_currentpacket, d->I3_input_buffer + 4, size );

   if( d->I3_currentpacket[size - 1] == '\r' || d->I3_currentpacket[size - 1] == '\0' )
   {
      d->I3_currentpacket[size - 1] = '\0'; // Normalize everything to a null terminator.
   }

   d->I3_currentpacket[size] = '\0'; // Don't forget to null your strings folks!

   // Shift the remaining data (if any).
   size_t remaining = d->I3_input_pointer - (size + 4);
   if( remaining > 0 )
   {
      memmove( d->I3_input_buffer, d->I3_input_buffer + size + 4, remaining );
   }
   d->I3_input_pointer = remaining;
}

#ifdef PACKET_CHECK
/*
 * function now deals with checking for validity of packets in
 * both the brace count and for null packets -Nopey
 */
char *i3_packet_check( char *ps )
{
   int iOperCount = 0, i = 0, j = 0;
   static char p[IPS];
   char last = 0;
   bool bFound = false, quotes = false, backslash = false;

   /* packet contains no data at all */
   if( ps[0] == '\0' )
   {
      i3bug( "{}: empty packet", __func__ );
      return nullptr;
   }
   
   /*
    * okay, a simple understanding of what this does --
    * it runs through the packet and counts the opening braces
    * and then adds them to the total count. At the same time
    * it also checks for the closing braces. If at the end
    * of this check the total count isn't 0 then the packet
    * is dead and doesn't go further
    */
   for( i = 0; ps[i] != '\0'; i++ )
   {
      switch( ps[i] )
      {
         /* opening brackets (add to count) */
         case '{':
         case '(':
         case '[':
            if( !quotes )
               iOperCount++;
            break;

         case '"':
            if( !backslash )
               quotes = !quotes;
            break;

         /* should fix that backslash problem */
         case '\\':
            backslash = !backslash;
            break;

         /* closing brackets (subtract from count) */
         case '}':
         case ')':
         case ']':
            if( !quotes )
               iOperCount--;
            break;

         default:
            break;
      }
   }

   /* too many opening brackets {([ */
   if( iOperCount > 0 )
   {
      i3bug( "{}: packet has too many opening brackets\ni3_packet_check(): {}", __func__, ps );
      return nullptr;
   }
   /* too many closing brackets })] */
   if( iOperCount < 0 )
   {
      i3bug( "{}: packet has too many closing brackets\ni3_packet_check(): {}", __func__, ps );
      return nullptr;
   }

   /* At the moment this is only vital to keep channel-admin from crashing the router.
    * So unless a good reason can be seen to use it on everything, I'm going to be
    * selective since this tends to mangle perfectly good packets sometimes. -- Samson
    */
   if( !strstr( ps, "channel-admin" ) )
      return ps;
   else
   {
      quotes = false;
      backslash = false;
      /* this is probably a kludge way to do this; but until someone else steps
       * up and changes it it should work correctly */
      for( i = 0, j = 0; ps[i] != '\0'; i++, j++ )
      {
         switch( last )
         {
            /* only problem here is that no matter what bracket is opening
            * the checker will check for a closing bracket -- 9 times out
            * of ten it will be the correct bracket -- if not, then there's
            * still something wrong with the packet, why would we have {) with
            * no closing curly bracket?
               */
            case '{':
            /* case '(':
            case '[': */
            {
               if( !quotes )
               {
                  /* here's the kludge part -- the protocol was written with null packets in
                  * mind, but they don't include a "", for a nullptr packet, it's just not there.
                  * A wrong idea I think, but since it's more of a pain in the ass to write
                  * a parser check/fix for that we're just going to add a "",  to the input
                  * packets that need it, and remove it for the output packets */
                  if( ( ps[i] == '}' ) /* || ( ps[i] == ')' ) || ( ps[i] == ']' ) */ )
                  {
                     bFound = true;
                     p[j++] = '"';
                     p[j++] = '"';
                     p[j++] = ',';
                  }
                  break;
               }
            }

            case '"':
               if( !backslash )
                  quotes = !quotes;
               break;

            /* should fix that backslash problem */
            case '\\':
               backslash = !backslash;
               break;

            default:
               break;
         }
         last = ps[i]; p[j] = last;
         continue;
      }

      /* we need to add the ending for the string because the for loop ends
       * before it can add this (I think?) lol */
      p[j] = '\0';

      /* report the bug to the bug stream -- keep records from where these packets
       * are originating from, maybe a client fix later on? Who the hell knows... */
      if( bFound )
      {
         i3bug( "{}: caught bad packet: {}", __func__, ps );
         i3log( "Fixed packet: {}", p );
      }
   }
   return p;
}
#endif

/*
 * Read the first field of an I3 packet and call the proper function to
 * process it. Afterwards the original I3 packet is completely messed up.
 */
void I3_parse_packet( descriptor_data *d )
{
   i3header *header = nullptr;
   char *ps = nullptr;
   char *next_ps = nullptr;
   char ptype[MIL];
   std::string ebuf;
   char *input_buffer = d->I3_currentpacket;

   if( input_buffer[0] != '(' || input_buffer[1] != '{' )
      return;
//   if( d->I3_currentpacket[0] != '(' || d->I3_currentpacket[1] != '{' )
//      return;

   if( packetdebug )
      i3log( "Packet received: {}", d->I3_currentpacket );

#ifdef PACKET_CHECK
   /* Nopey: alright then; we need to check these packets for the {[( */
   //ps = i3_packet_check( d->I3_currentpacket );
   ps = i3_packet_check( input_buffer );
   if( ps == nullptr || ps[0] == '\0' || !str_cmp( ps, "" ) )
      return;
#else
//   ps = d->I3_currentpacket;
   ps = input_buffer;
#endif

   ps += 2;
   I3_get_field( ps, &next_ps );
   I3_remove_quotes( &ps );
   strlcpy( ptype, ps, MIL );

   header = I3_get_header( &ps );

   /* Broadcasts or targeted packets not destined for the router */
   if( str_cmp( header->target_mudname, I3_THISMUD ) && d->mud )
   {
      if( !str_cmp( ptype, "channel-m" ) )
      {
         process_channel_m( d->mud, header, ps, true );
         deleteptr( header );
         return;
      }
      if( !str_cmp( ptype, "channel-e" ) )
      {
         process_channel_e( d->mud, header, ps, true );
         deleteptr( header );
         return;
      }
      if( !str_cmp( ptype, "channel-t" ) )
      {
         process_channel_t( d->mud, header, ps, true );
         deleteptr( header );
         return;
      }
      if( !str_cmp( ptype, "ucache-update" ) )
      {
         process_ucache_update( d->mud, header, ps );
         deleteptr( header );
         return;
      }
      if( !str_cmp( ptype, "locate-req" ) )
      {
         process_locate_req( d->mud, header, ps );
         deleteptr( header );
         return;
      }

      /* If we get here, it's not a known broadcast packet. Send it to it's target if possible. */
      if( str_cmp( header->target_mudname, "0" ) )
      {
         i3_mud *tmud;

         if( ( tmud = find_mud( header->target_mudname ) ) != nullptr )
         {
            /* Special case for different protocol version [not currently working properly??] *
            if( !str_cmp( ptype, "locate-reply" ) )
            {
               process_locate_reply( tmud, header, ps );
               deleteptr( header );
               return;
            } */

            if( !tmud->desc )
            {
               ebuf = std::format( "{} is not currently connected.", header->target_mudname );
               I3_send_error( d->mud, header->originator_username, "unk-dst", ebuf, "" );
            }
            else
            {
               I3_write_header( tmud, ptype, header->originator_mudname, header->originator_username, header->target_mudname, header->target_username );
               I3_write_buffer( tmud, ps );
            }
            deleteptr( header );
            return;
         }
         else
         {
            ebuf = std::format( "No such mud: {}", header->target_mudname );
            I3_send_error( d->mud, header->originator_username, "unk-dst", ebuf, "" );
            deleteptr( header );
            return;
         }
      }

      /*
       * Although I strongly disagree with this - it seems the other routers allow it.
       * My main objection is that muds who don't support whatever you're doing will throw errors.
       * This gets spammy, and I see no valid reason for why the router should then have to bear
       * the burden of all the extra error message traffic. My experiment with the old buddy-list packets
       * being a prime example of why "just looking at channel packets and blindly forwarding the others"
       * is a BAD THING(tm). If this becomes a serious issue at some point down the road then I will undo this.
       */
      i3log( "Unknown broadcast packet received: {} from {} containing: {}", ptype, header->originator_mudname, ps );
      for( auto* mud : mud_list )
      {
         if( mud == d->mud )
            continue;

         I3_write_header( mud, ptype, header->originator_mudname, header->originator_username, header->target_mudname, header->target_username );
         I3_write_buffer( mud, ps );
      }
      deleteptr( header );
      return;

      /* This is the old blocking code for unsupported broadcasts - keep this should we need it again.
      snprintf( ebuf, MSL, "Unknown broadcast packet: %s", ptype );
      I3_send_error( d->mud, header->originator_username, "unk-type", ebuf, nullptr );
      i3log( "Unknown broadcast packet received: {} from {} containing: {}", ptype, header->originator_mudname, ps );
      deleteptr( header );
      return; */
   }

   /* Everything from this point down is processed directly since it was targeted for the router */
   if( !str_cmp( ptype, "startup-req-3" ) || !str_cmp( ptype, "startup-req-2" ) || !str_cmp( ptype, "startup-req-1" ) )
   {
      process_startup_req( d, header, ptype, ps );
      deleteptr( header );
      return;
   }

   if( !d->mud )
   {
      i3bug( "{}: Descriptor has nullptr mud! Closing link to {}", __func__, d->host );
      close_socket( d );
      deleteptr( header );
      return;
   }

   if( !str_cmp( ptype, "shutdown" ) )
   {
      process_shutdown( d->mud, header, ps );
      deleteptr( header );
      return;
   }

   if( !str_cmp( ptype, "chan-filter-reply" ) )
   {
      process_channel_filter( d->mud, header, ps );
      deleteptr( header );
      return;
   }

   if( !str_cmp( ptype, "channel-listen" ) )
   {
      process_channel_listen( d->mud, header, ps );
      deleteptr( header );
      return;
   }

   if( !str_cmp( ptype, "channel-add" ) )
   {
      process_channel_add( d->mud, header, ps );
      deleteptr( header );
      return;
   }

   if( !str_cmp( ptype, "channel-remove" ) )
   {
      process_channel_remove( d->mud, header, ps );
      deleteptr( header );
      return;
   }

   if( !str_cmp( ptype, "channel-admin" ) )
   {
      process_channel_admin( d->mud, header, ps );
      deleteptr( header );
      return;
   }

   if( !str_cmp( ptype, "chan-adminlist" ) )
   {
      process_channel_adminlist( d->mud, header, ps );
      deleteptr( header );
      return;
   }

   /* silently drop error packets sent to the router */
   if( str_cmp( ptype, "error" ) )
   {
      ebuf = std::format( "Unknown or unsupported packet: {}", ptype );
      I3_send_error( d->mud, header->originator_username, "unk-type", ebuf, "" );
      i3log( "{} is unsupported. Target: {} Data: {}", ptype, header->target_mudname, ps );
   }
   deleteptr( header );
}

void set_alarm( long seconds )
{
   alarm( seconds );
}

static void graceful_exit( int sig )
{
   i3log( "{} router killed from shell.", I3_THISMUD );

   i3_sendtoall( nullptr, "router_shutdown", "" );
   save_mudlist();
   save_channels();
   i3log( "{} muds saved.", mud_count );
   i3log( "{} channels saved.", chan_count );
   i3log( "Bytes received: {}", bytes_received );
   i3log( "Bytes sent    : {}", bytes_sent );
   i3log( "Events served : {}", events_served );
   i3log( "All connections terminated. Rebooting shortly." );
   close( I3_socket );
   sleep( 2 );
   write_shutdown_file();
   std::exit( EXIT_SUCCESS );
}

void close_socket( descriptor_data *dclose )
{
   i3_mud *mud = nullptr;

   if( !dclose )
   {
      i3bug( "{}: Called with nullptr descriptor!", __func__ );
      return;
   }

   mud = dclose->mud;

   if( mud )
   {
      for( auto* channel : channel_list )
      {
         for( auto it = channel->listeners.begin(  ); it != channel->listeners.end(  ); )
         {
            i3_listener *listener = *it;
            ++it;

            if( !str_cmp( mud->name, listener->name ) )
            {
               channel->listeners.remove( listener );
               deleteptr( listener );
            }
         }
      }
      if( mud->status == -1 )
         mud->status = 0;
      mud->desc = nullptr;
      cancel_event( ev_sendmudlist, mud );
      cancel_event( ev_sendchanlist, mud );
      i3_sendtoall( mud, "mudlist", "" );
   }

   deleteptr( dclose );
}

/* Save current ban list. Short, simple. */
void savebans( void )
{
   std::ofstream stream( std::filesystem::path{BAN_FILE} );
   if( !stream.is_open() )
   {
      i3bug( "{}: Cannot open {} for writing: {}", __func__, BAN_FILE, std::strerror(errno) );
      return;
   }

   stream << "#BANS\n\n";

   for( auto* b : ban_list )
   {
      stream << "#I3BAN\n";
      stream << std::format( "Host {}\n", b->host );
      stream << "End\n\n";
   }

   stream << "#END\n";
   stream.close();
   if( stream.fail() )
      i3bug( "{}: Error occurred after closing {}: ", __func__, BAN_FILE, std::strerror(errno) );
}

void delban( std::string_view host )
{
   for( auto it = ban_list.begin(  ); it != ban_list.end(  ); )
   {
      ban_data *b = *it;
      ++it;

      if( !str_cmp( host, b->host ) )
      {
         deleteptr( b );

         savebans();
         return;
      }
   }
}

void addban( std::string_view host )
{
   ban_data *ban = new ban_data;
   ban->host = host;
   ban_list.push_back( ban );

   savebans();
}

void read_ban( ban_data *ban, std::ifstream & stream )
{
   std::string key;
   while( stream >> key )
   {
      if( key == "Host" )
      {
         ban->host = i3fread_line( stream, '\n' );
      }
      else if( key == "End" )
         return;
      else
      {
         i3bug( "{}: Bad section '{}' - skipping.", __func__, key );
         i3fread_to_eol( stream );
      }
   }
}

void load_bans( void )
{
   ban_list.clear();

   i3log( "Loading ban list..." );

   std::ifstream stream( std::filesystem::path{BAN_FILE} );
   if( !stream.is_open() )
   {
      i3bug( "{}: Cannot open {} for reading: {}", __func__, BAN_FILE, std::strerror(errno) );
      return;
   }

   std::string key;
   while( stream >> key )
   {
      if( key == "#I3BAN" )
      {
         ban_data *ban = new ban_data;

         read_ban( ban, stream );

         if( ban->host.empty() )
            deleteptr( ban );
         else
            ban_list.push_back( ban );
         continue;
      }
      else if( key == "#END" )
         break;
      else
      {
         i3bug( "{}: Bad section '{}' - skipping.", __func__, key );
         i3fread_to_eol( stream );
      }
   }
   stream.close();
}

bool check_bans( descriptor_data *dnew )
{
   for( auto* ban : ban_list )
   {
      if( !str_cmp( ban->host, dnew->host ) )
         return true;
   }
   return false;
}

void new_descriptor( int new_desc )
{
   struct sockaddr_in6 sock;
   char host_buf[NI_MAXHOST];
   socklen_t size;

   size = sizeof( sock );

   set_alarm( 20 );
   alarm_section = "new_descriptor: accept";

   int desc = accept4( new_desc, (struct sockaddr *)&sock, &size, SOCK_NONBLOCK | SOCK_CLOEXEC );

   if( desc < 0 )
   {
      // EAGAIN/EWOULDBLOCK is normal if something else snatched the connection.
      if( errno != EAGAIN && errno != EWOULDBLOCK )
      {
         perror("new_descriptor: accept4");
      }
      set_alarm(0);
      return;
   }

   set_alarm( 20 );
   alarm_section = "new_descriptor: after accept";

   descriptor_data *dnew = new descriptor_data;
   dnew->descriptor = desc;

   if( getnameinfo( (struct sockaddr *)&sock, size, host_buf, sizeof(host_buf), NULL, 0, NI_NUMERICHOST) == 0 )
   {
      /*
       * If using a dual-stack socket, IPv4 addresses often appear as
       * ::ffff:192.168.1.1. We can strip the prefix if desired,
       * but getnameinfo provides the clean format by default.
       */
      std::string ip = host_buf;

      // Optional: Strip "::ffff:" prefix if you strictly want IPv4 notation.
      if( ip.compare( 0, 7, "::ffff:" ) == 0 )
      {
         dnew->host = ip.substr(7);
      }
      else
      {
         dnew->host = ip;
      }
   }
   else
   {
      dnew->host = "Unknown";
   }

   if( desc == 0 )
   {
      i3bug( "{}: ALERT! Assigning socket 0! BAD BAD BAD! Host: {}", __func__, dnew->host );
      deleteptr( dnew );
      set_alarm( 0 );
      return;
   }

   i3log( "Incoming connection: {}", dnew->host );
   if( check_bans( dnew ) )
   {
      i3log( "Banned mud attempted to connect from {}", dnew->host );
      deleteptr( dnew );
      set_alarm( 0 );
      return;
   }

   dnew->client_port = ntohs( sock.sin6_port );
   descriptor_list.push_back( dnew );
   set_alarm( 0 );
   i3log( "Connection complete: {}", dnew->host );
}

void poll_update( int ctrl )
{
   control_has_input = false;
   control_has_exception = false;

   // Build the vector of descriptors to monitor.
   std::vector<struct pollfd> poll_fds;

   // Main listening socket.
   poll_fds.push_back( { ctrl, POLLIN | POLLERR | POLLHUP, 0 } );

   // MUD sockets.
   for( auto* d : descriptor_list )
   {
      short events = POLLIN | POLLERR | POLLHUP;

      // Only look for outbound capacity if they actually have data to send.
      if( d->output_buffer.length() > 0 )
         events |= POLLOUT;

      poll_fds.push_back( { d->descriptor, events, 0 } );
   }

   // Non-blocking poll call.
   if( poll( poll_fds.data(), poll_fds.size(), 0 ) < 0 )
   {
      perror( "poll_update: poll" );
      std::exit( EXIT_FAILURE );
   }

   size_t idx = 0;

   // Control socket results.
   auto& ctrl_pfd = poll_fds[idx++];
   control_has_input = ( ctrl_pfd.revents & POLLIN );
   control_has_exception = ( ctrl_pfd.revents & (POLLERR | POLLHUP | POLLNVAL) );

   // MUD socket results.
   for( auto* d : descriptor_list )
   {
      auto& pfd = poll_fds[idx++];

      d->has_exception    = ( pfd.revents & (POLLERR | POLLHUP | POLLNVAL) );
      d->has_input_ready  = ( pfd.revents & POLLIN );
      d->has_output_ready = ( pfd.revents & POLLOUT );
   }
}

void accept_new( int ctrl )
{
   if( control_has_exception )
   {
      i3bug( "{}: Exception raised on controlling descriptor {}", __func__, ctrl );
      return;
   }

   if( control_has_input )
      new_descriptor( ctrl );
}

// Synchronize to a clock.
void pulse_sync( )
{
   using namespace std::chrono_literals;

   // Use steady_clock to ensure timing is monotonic and unaffected by NTP syncs.
   static auto next_pulse = std::chrono::steady_clock::now();

   // Calculate the duration of one pulse based on pulses per second. Defaults to 4 per second.
   auto pulse_duration = std::chrono::nanoseconds( 1'000'000'000 / 4 );
   next_pulse += pulse_duration;

   // Sleep until the exact time the next pulse should occur.
   // If the system is running behind, this will return immediately.
   std::this_thread::sleep_until( next_pulse );

   // Update global time
   current_time = std::chrono::system_clock::now();
}

void purge_unused_channels( void )
{
   for( auto it = channel_list.begin(  ); it != channel_list.end(  ); )
   {
      i3_mud *mud;

      i3_channel *channel = *it;
      ++it;

      /* Preserve the channel for as long as the host mud remains valid */
      if( ( mud = find_mud( channel->host_mud ) ) != nullptr )
      {
         channel->purge_time += std::chrono::days( 30 );
         continue;
      }
      if( channel->purge_time == current_time )
         destroy_I3_channel( channel );
   }
}

void purge_expired_muds( void )
{
   for( auto it = mud_list.begin(  ); it != mud_list.end(  ); )
   {
      i3_mud *mud = *it;
      ++it;

      if( mud->mcount > -1 && mud->mcount < mud_count && mud->desc != nullptr )
         send_mudlist( mud );

      if( mud->purge_time == current_time && mud->desc == nullptr )
         destroy_I3_mud( mud );
   }
}

void process_output( void )
{
   for( auto it = descriptor_list.begin(  ); it != descriptor_list.end(  ); )
   {
      descriptor_data *d = *it;
      ++it;

      if( d->output_buffer.length() > 0 && d->has_output_ready )
      {
         I3_send_packet( d );
      }
   }
}

void process_input( void )
{
   int ret;

   // Kick out descriptors with raised exceptions, then check for input.
   for( auto it = descriptor_list.begin(  ); it != descriptor_list.end(  ); )
   {
      descriptor_data *d = *it;
      ++it;

      if( d->has_exception )
      {
         close_socket( d );
         continue;
      }
      else
      {
         if( d->has_input_ready )
         {
            ret = read( d->descriptor, d->I3_input_buffer + d->I3_input_pointer, MSL );
            if( !ret || ( ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK ) )
            {
               d->has_output_ready = false;

               if( ret < 0 )
                  i3log( "Read error on socket." );
               else
                  i3log( "EOF encountered on socket read." );
               i3log( "Closing link to {}", ( d->mud && !d->mud->name.empty() ) ? d->mud->name : d->host );
               close_socket( d );
               continue;
            }
            if( ret < 0 ) /* EAGAIN */
               continue;

            d->I3_input_pointer += ret;
            bytes_received += ret;
            if( packetdebug )
            {
               i3log( "Bytes received: {}", ret );
               i3log( "Input received: {}", d->I3_input_buffer + 4 );
            }
         }

         int offset = 0;

         while( ( d->I3_input_pointer - offset ) >= 4 )
         {
            long size;

            // Read it
            I3_read_packet( d );

            // Parse it
            I3_parse_packet( d );

            // Send response - need to do this here to handle "packet storms" like channel-listen.
            I3_send_packet( d );

            // Advance the offset by packet size
            offset += ( size + 4 );
         }

         // After we've processed all full packets in the buffer, move the leftover (incomplete) data to the front
         if( offset > 0 )
         {
            int remaining = d->I3_input_pointer - offset;
            if( remaining > 0 )
               memmove( d->I3_input_buffer, d->I3_input_buffer + offset, remaining );
            d->I3_input_pointer = remaining;
         }
      }
   }
}

void i3loop( void )
{
   i3log( "Beginning main idle loop." );

   while( !router_down )
   {
      poll_update( I3_socket );

      accept_new( I3_socket );

      // If no descriptors are present, why bother processing input for them?
      if( !descriptor_list.empty() )
         process_input();

      run_events( current_time );

      purge_expired_muds();

      purge_unused_channels();

      // If no descriptors are present, why bother processing output for them?
      if( !descriptor_list.empty() )
         process_output();

      pulse_sync();
   }
   // End of main loop.
   // Returns back to 'main', and will result in router shutdown.
}

int i3startup( void )
{
   std::string port_str = std::to_string( this_mud->player_port );
   struct addrinfo hints{}, *res;
   int sockfd = -1;

   hints.ai_family = AF_INET6;       // IPv6
   hints.ai_socktype = SOCK_STREAM;  // TCP
   hints.ai_flags = AI_PASSIVE;      // For bind()

   i3log( "Initializing I3 router: {} at {}", I3_THISMUD, I3_THISIP );

   if( int err = getaddrinfo( nullptr, port_str.c_str(), &hints, &res ) )
   {
      throw std::runtime_error( gai_strerror( err ) );
   }

   sockfd = socket( res->ai_family, res->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, res->ai_protocol );
   if( sockfd < 0 )
   {
      freeaddrinfo( res );
      throw std::system_error( errno, std::generic_category(), "socket" );
   }

   int opt = 1;

   // Set V6ONLY to 0 to allow dual-stack (IPv4 and IPv6)
   int v6only = 0;
   setsockopt( sockfd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof( v6only ) );

   setsockopt( sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt) );
   setsockopt( sockfd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt) );
   setsockopt( sockfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt) );

   i3log( "Binding port {} for incoming connections...", this_mud->player_port );
   if( bind( sockfd, res->ai_addr, res->ai_addrlen ) < 0 )
   {
      close( sockfd );
      freeaddrinfo( res );
      throw std::system_error( errno, std::generic_category(), "bind" );
   }

   freeaddrinfo( res );

   if( listen( sockfd, 50 ) < 0 )
   {
      close( sockfd );
      throw std::system_error( errno, std::generic_category(), "listen" );
   }

   i3log( "I3 router initialized. Startup complete." );
   return sockfd;
}

void check_mudsfile( void )
{
   ban_data *b = nullptr;
   i3_mud *mud = nullptr;
   char c = '\0';
   bool found = false;

   std::ifstream stream( std::filesystem::path{UPDATE_FILE} );
   if( !stream.is_open() )
      return;

   std::filesystem::remove( UPDATE_FILE ); // Won't stop you from reading the rest of it - keeps it from causing crash loop.

   while( ( c = i3fread_letter( stream ) ) )
   {
      if ( c == '#' )
      {
         std::string host = i3fread_line( stream, '\n' );

         for( auto* ban : ban_list )
         {
            if( !str_cmp( host, b->host ) )
            {
               b = ban;
               break;
            }
         }

         if( b )
         {
            i3log( "Ban listed in a \'#\' line in the update file already exists." ); 
            continue;
         }
         i3log( "Adding ban for {} via autoupdate file.", host );
         addban( host );
         savebans();

         for( auto it = descriptor_list.begin(  ); it != descriptor_list.end(  ); )
         {
            descriptor_data *d = *it;
            ++it;

            if( !str_cmp( d->host, host ) )
            {
               destroy_I3_mud( d->mud );
               close_socket( d );
            }
         }
         continue;
      }
      else if ( c == '$' )
      {
         std::string host = i3fread_line( stream, '\n' );

         for( auto* ban : ban_list )
         {
            if( !str_cmp( host, ban->host ) )
            {
               b = ban;
               break;
            }
         }

         if( !b )
         {
            i3log( "Ban listed in a \'$\' line in the update file could not be found." ); 
            continue;
         }
         i3log( "Removing ban for {} via autoupdate file.", host );
         delban( host );
         continue;
      }
      else if ( c == '*' )
      {
         std::string chan = i3fread_line( stream, '\n' );
         found = false;

         for( auto it = channel_list.begin(  ); it != channel_list.end(  ); )
         {
            i3_channel *channel = *it;
            ++it;

            if( !str_cmp( chan, channel->name ) )
            {
               found = true;
               i3log( "Removing channel {} via autoupdate file.", chan );
               destroy_I3_channel( channel );
               continue;
            }
            if( !str_cmp( chan, channel->host_mud ) )
            {
               found = true;
               i3log( "Removing channel {} hosted by {} via autoupdate file.", channel->name, chan );
               destroy_I3_channel( channel );
            }
         }

         if( !found )
         {
            i3log( "Channel or channel host listed in a \'*\' line in the update file could not be found." ); 
            continue;
         }
         save_channels();
         continue;
      }
      else if ( c == '-' )
      {
         std::string name = i3fread_line( stream, '\n' );

         for( auto* muds : mud_list )
         {
            if( !str_cmp( name, muds->name ) )
            {
               mud = muds;
               break;
            }
         }

         if( !mud )
         {
            i3log( "Connection referenced in a \'-\' line in the update file could not be found." );
            continue;
         }
         i3log( "Removing connection for {} via autoupdate file.", mud->name );
         if( mud->desc )
            close_socket( mud->desc );
         destroy_I3_mud( mud );
         save_mudlist();
         continue;
      }
      else
      {
         i3log( "Bad \'start of line\' character found in the update file." );

         i3fread_to_eol( stream );
      }
   }
   stream.close();
}

void ev_mudconnections( void *data )
{
   check_mudsfile();
   add_event( 15, ev_mudconnections, nullptr );
}

int main( void )
{
   current_time = std::chrono::system_clock::now();
   bytes_received = 0;
   bytes_sent = 0;

   signal( SIGPIPE, SIG_IGN );
   signal( SIGTERM, graceful_exit ); /* Catch kill signals */

   i3log( "---------------------[ Boot Log ]--------------------" );

   i3log( "Initializing random number generator." );

   if( !I3_read_config() )
   {
      i3log( "OUCH! I wasn't able to read my config!" );
      write_shutdown_file();
      std::exit( EXIT_FAILURE );
   }

   for( int x = 0; x < MAX_CONN; x++ )
      mlist_array[x] = nullptr;

   I3_loadmudlist();

   try
   {
      if( ( I3_socket = i3startup() ) < 0 )
      {
         i3log( "OUCH! I3 startup failed with code: {}", I3_socket );
         write_shutdown_file();
         std::exit( EXIT_FAILURE );
      }
   }
   catch( const std::exception & e )
   {
      i3log( "OUCH! Exception during startup: {}", e.what() );
      write_shutdown_file();
      std::exit( EXIT_FAILURE );
   }
   catch( ... )
   {
      i3log( "OUCH! An unknown exception occurred during startup!" );
      write_shutdown_file();
      std::exit( EXIT_FAILURE );
   }

   I3_loadchannels();

   // Periodically save the channel list. Would spam the disk to do this on every pulse.
   add_event( 300, ev_savechanlist, nullptr );

   // Initiate autoupdate system.
   add_event( 15, ev_mudconnections, nullptr );

   i3loop();

   // If we get here, something bad happened.
   i3log( "{}: ACK! Something made me close myself!", __func__ );

   save_mudlist();
   save_channels();

   fflush( stderr );
   close( I3_socket );
   std::exit( EXIT_SUCCESS );
}
