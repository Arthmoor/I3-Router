/*
 * Intermud-3 Router Copyright (c)2003 by Roger Libiez (Samson)
 *
 * Version 2.0
 *
 * See License/License.txt for legal info.
 */

inline constexpr std::string_view CHANNEL_FILE = "./config/i3.channels";
inline constexpr std::string_view CONFIG_FILE  = "./config/i3.config";
inline constexpr std::string_view MUD_FILE     = "./config/i3.muds";
inline constexpr std::string_view BAN_FILE     = "./config/i3.ban";
inline constexpr std::string_view UPDATE_FILE  = "./update";

constexpr int IPS = 2097152;   // Maximum incoming packet size: Per protocol specs at https://wotf.org/specs/mudmode.html
constexpr int MSL = 16384;     // Maximum large string.
constexpr int MIL = 8192;      // Maximum small string.
constexpr int MAX_CONN = 1000; // Maximum allowed muds in the list.

class i3_mud;

/*
 * Descriptor (channel) structure.
 */
class descriptor_data
{
public:
   descriptor_data( const descriptor_data & ) = delete;
   descriptor_data & operator = ( const descriptor_data & ) = delete;

   descriptor_data(  );
   ~descriptor_data(  );

   i3_mud *mud = nullptr;
   std::string host;
   std::string output_buffer;
   int client_port = 0;
   int descriptor = 0;
   char I3_input_buffer[IPS]{0};
   char I3_currentpacket[IPS]{0};
   long I3_input_pointer = 0;
   bool has_input_ready  = false;
   bool has_output_ready = false;
   bool has_exception    = false;
};

inline bool control_has_input     = false;
inline bool control_has_exception = false;

class ban_data
{
 public:
   ban_data( const ban_data & ) = delete;
   ban_data & operator = ( const ban_data & ) = delete;

   ban_data(  );
   ~ban_data(  );

   std::string host;
};

class i3header
{
 public:
   i3header( const i3header & ) = delete;
   i3header & operator = ( const i3header & ) = delete;

   i3header(  );
   ~i3header(  );

   char originator_mudname[256]{0};
   char originator_username[256]{0};
   char target_mudname[256]{0};
   char target_username[256]{0};
};

class i3_listener
{
 public:
   i3_listener( const i3_listener & ) = delete;
   i3_listener & operator = ( const i3_listener & ) = delete;

   i3_listener(  );
   ~i3_listener(  );

   std::string name;
};

class mlist
{
 public:
   mlist( const mlist & ) = delete;
   mlist & operator = ( const mlist & ) = delete;

   mlist(  );
   ~mlist(  );

   std::string name;
};

class i3_channel
{
 public:
   i3_channel( const i3_channel & ) = delete;
   i3_channel & operator = ( const i3_channel & ) = delete;

   i3_channel(  );
   ~i3_channel(  );

   std::list<i3_listener *> listeners;
   std::list<mlist *> m_listeners;
   std::chrono::system_clock::time_point purge_time;
   std::string host_mud;
   std::string name;
   int status = 0;
};

class i3_mud
{
 public:
   i3_mud( const i3_mud & ) = delete;
   i3_mud & operator = ( const i3_mud & ) = delete;

   i3_mud(  );
   ~i3_mud(  );

   class descriptor_data *desc = nullptr;
   int version; /* Protocol version - currently 1/2/3 */
   std::chrono::system_clock::time_point purge_time;

   /* Stuff for the first mapping set */
   int status = 0;
   std::string name;
   std::string ipaddress;
   std::string mudlib;
   std::string base_mudlib;
   std::string driver;
   std::string mud_type;
   std::string open_status;
   std::string admin_email;
   std::string telnet;

   int player_port = 0;
   int imud_tcp_port = 0;
   int imud_udp_port = 0;
   int password = 0;
   int mudlist_id = 0;
   int chanlist_id = 0;

   bool tell = false;
   bool beep = false;
   bool emoteto = false;
   bool who = false;
   bool finger = false;
   bool locate = false;
   bool channel = false;
   bool news = false;
   bool mail = false;
   bool file = false;
   bool auth = false;
   bool ucache = false;

   int smtp = 0;
   int ftp = 0;
   int nntp = 0;
   int http = 0;
   int pop3 = 0;
   int rcp = 0;
   int amrcp = 0;

   /* Stuff for the second mapping set - can be added to as indicated by i3log messages for missing keys */
   std::string banner;  // Careful - this is allowed to have newlines in it per the protocol.
   std::string web;
   std::string time;
   std::string daemon;
   int jeamland = 0;

   /* Local router */
   std::string routerIP;
   std::string routerName;

   /* Not part of packet data */
   int mcount = 0;
};

i3_mud *mlist_array[MAX_CONN];

class event_info
{
 public:
   event_info( const event_info & ) = delete;
   event_info & operator = ( const event_info & ) = delete;

   event_info(  );
   ~event_info(  );

   void ( *callback ) ( void *data );           // Function the event will call when it runs.
   void *data;                                  // Data used as arguments for the event function.
   std::chrono::system_clock::time_point when;  // When this event is scheduled for.
};

template < typename T > void deleteptr( T * &ptr )
{
   delete ptr;
   ptr = nullptr;
}
