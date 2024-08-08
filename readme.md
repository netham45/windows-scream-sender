# ScreamSender
This is a usermode alternative to the Windows Scream driver that sends audio over the network. It implements the UDP protocol from the Scream Windows Audio driver. It is intended to be used with ScreamRouter. It works by capturing audio to an existing sound card instead of providing it's own.

## Usage

* Build with Visual Studio
* Run with `ScreamSender.exe <IP> [Port=16401] [-m]`
  * -m for multicast


To be compatible with a default Scream receiver:

* `ScreamSender.exe 239.255.77.77 4010 -m`