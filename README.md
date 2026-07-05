Intermud-3 Standalone Router for I3 Networks
============================================

This code allows you to set up your own Intermud-3 router for clients to connect to.
It is lightweight and does not require any LPMud libs or drivers to run.

Once online, any MUD following the standard should have no trouble connecting to it.

This package has been released as a community service for those who are
interested in how the I3 system works. It is a crude pile of code, but it
does the job, and is fairly stable. It has handled as many as 30 active connections.
Most of the time the router which resulted from its development sat idle and did not do
much of anything. If you're looking to test a client adaptation, you would be encouraged
to do so by running a copy of this router code on your own server and connect to it there.
This spares the main network the additional overhead of your testing activities.

If you are instead looking for somewhere to connect an existing client, I would suggest
connecting to the *dalet router instead. *dalet is also known by most as the "main" router
and averages some 100 connections daily.

Changes for version 2.0
-----------------------

Compiler changed to g++

The TRUE/FALSE macros have been swapped with actual true/false booleans.

The UMIN/UMAX/URANGE macros have been removed and the one place that used UMAX is now using std::max instead.

The LOWER and UPPER macros have been removed.

The random number generator has been swapped to use the std::mt19937 algorithm (Mersenne Twister).

All compiler errors and warnings for GCC 13.3 using -std=c++23 have been addressed.

Time handling uses std::chrono instead of time_t for better precision.

All printf style functions use std::format instead.

All uses of the LINK/UNLINK macros eliminated. Lists now use std::list.

All file I/O is now using std::ifstream and std::ofstream.

Made the I3_read_packet() function safer. It used to attempt to ingest infinitely large packets.

Installation
============

1. Unpack the contents of the archive on the server you intend to run this from.

2. Run make.

3. After a few seconds, you should have a compiled router file.

4. Make sure your configuration is set up right. The i3.config file should have your IP and chosen router name.

5. Run the startup script:

./i3start.sh &

6. That's it. You should now be able to have a client connect to it to make sure it's working.

Do you need a client for a DIKU derived codebase? You can find that here:
