.ND "March 10, 1999"
.TL
Built-in Kernel Debugger for Linux
.AU "Scott Lurndal" SL 8U500 OS 33158
.AT "Member Technical Staff"
.AF "Silicon Graphics, Inc."
.MT 2
.AS
These programmer notes describe the built-in kernel debugger 
for linux.  
.AE
.H 1 "Overview"
This document describes the built-in kernel debugger available
for linux.   This debugger allows the programmer to interactivly
examine kernel memory, disassemble kernel functions, set breakpoints
in the kernel code and display and modify register contents. 
.P
A symbol table is included in the kernel image which enables all
symbols with global scope (including static symbols) to be used
as arguments to the kernel debugger commands.
.H 1 "Getting Started"
To include the kernel debugger in a linux kernel, use a
configuration mechanism (e.g. xconfig, menuconfig, et. al.)
to enable the \fBCONFIG_KDB\fP option.   Additionally, for accurate
stack tracebacks, it is recommended that the \fBCONFIG_KDB_FRAMEPTR\fP
option be enabled.   \fBCONFIG_KDB_FRAMEPTR\fP changes the compiler
flags so that the frame pointer register will be used as a frame
pointer rather than a general purpose register.   
.P
After linux has been configured to include the kernel debugger, 
make a new kernel with the new configuration file (a make clean
is recommended before making the kernel), and install the kernel
as normal.
.P
When booting the new kernel using \fIlilo\fP(1), the 'kdb' flag
may be added after the image name on the \fBLILO\fP boot line to
force the kernel to stop in the kernel debugger early in the 
kernel initialization process.     If the kdb flag isn't provided, 
then kdb will automatically be invoked upon system panic or 
when the 
\fBPAUSE\fP
key is used from the keyboard.
.P
Kdb can also be used via the serial port.  Set up the system to 
have a serial console (see \fIDocumentation/serial-console.txt\fP).
The \fBControl-A\fP key sequence on the serial port will cause the
kernel debugger to be entered with input from the serial port and
output to the serial console.
.H 2 "Basic Commands"
There are several categories of commands available to the
kernel debugger user including commands providing memory
display and modification, register display and modification,
instruction disassemble, breakpoints and stack tracebacks.
.P
The following table shows the currently implemented commands:
.DS
.TS
box, center;
l | l
l | l.
Command	Description
_
bc	Clear Breakpoint
bd	Disable Breakpoint
be	Enable Breakpoint
bl	Display breakpoints
bp	Set or Display breakpoint
bpa	Set or Display breakpoint globally
cpu	Switch cpus
env	Show environment
go	Restart execution
help	Display help message
id	Disassemble Instructions
ll	Follow Linked Lists
md	Display memory contents
mds	Display memory contents symbolically
mm	Modify memory contents
reboot	Reboot the machine
rd	Display register contents
rm	Modify register contents
set	Add/change environment variable
.TE
.DE
.P
Further information on the above commands can be found in
the appropriate manual pages.   Some commands can be abbreviated, such
commands are indicated by a non-zero \fIminlen\fP parameter to 
\fBkdb_register\fP; the value of \fIminlen\fP being the minimum length
to which the command can be abbreviated (for example, the \fBgo\fP
command can be abbreviated legally to \fBg\fP).
.P
If an input string does not match a command in the command table, 
it is treated as an address expression and the corresponding address
value and nearest symbol are shown.
.H 1 Writing new commands
.H 2 Writing a built-in command
.H 2 Writing a modular command
