# pico_ps2_diagnostic_tool
The pico_ps2_diagnostic_tool is designed to capture and replay signals on a PS/2 interface, specifically targeting the DATA and CLOCK lines.

I developed this project to assist my customers in providing more detailed reports about the behavior of problematic PS/2-to-USB adapters, including glitches and other anomalies, for my okhi project (Open Keylogger Hardware Implant – USB & PS/2 Keyboards) https://github.com/therealdreg/okhi

![](stuff/withcables.jpg)

A "capture" refers to a sequence of GPIO readings taken at short intervals, effectively logging the entire state timeline of the PS/2 pins during the recording session. These captures can be replayed to emulate the original signals, stored in flash memory, deleted, or exported for further analysis.

The tool supports multiple captures in flash memory, enabling operations such as recording, replaying, deleting, importing, and exporting. This makes it a versatile diagnostic solution for analyzing PS/2 device signals or debugging signal issues.

The captures can be exported from the Raspberry Pi Pico flash to COM port and new captures can be imported from the PC to the Raspberry Pi Pico internal flash through the COM port.

Additionally, the tool includes a glitch detector that monitors and flags extremely short pulses on the PS/2 clock line. (made by some PS2-USB adapters)

All you need is a Raspberry Pi Pico (or my okhi implant) to record PS/2 signals:
- DATA on GPIO20
- CLOCK on GPIO21

When needed, you can replay these signals via:
- DATA on GPIO1
- CLOCK on GPIO0

# Related

- https://github.com/therealdreg/okhi
- https://github.com/therealdreg/pico-ps2-sniffer
