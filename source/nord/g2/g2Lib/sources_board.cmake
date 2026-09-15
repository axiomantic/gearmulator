# Source list for the board track. Owned by the board track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree.

list(APPEND G2LIB_SOURCES memoryMap.cpp)
list(APPEND G2LIB_SOURCES sim.cpp)
list(APPEND G2LIB_SOURCES panel.cpp)
list(APPEND G2LIB_SOURCES latches.cpp)
list(APPEND G2LIB_SOURCES hdi08Decode.cpp)

# Header-only. Listed here for the IDE source group, like flash.h below.
list(APPEND G2LIB_SOURCES anomalyLog.h)

list(APPEND G2LIB_SOURCES firmwareExtract.cpp)

list(APPEND G2LIB_SOURCES
	flash.h
	flash.cpp
)

# ----------------- the two-tier interrupt controller
list(APPEND G2LIB_SOURCES
	interruptController.h
	interruptController.cpp
)

# ----------------- the bootstrap ROM
list(APPEND G2LIB_SOURCES
	hdi08Bootstrap.h
	hdi08Bootstrap.cpp
)

# ----------------- the HDI08 host-port adapter
list(APPEND G2LIB_SOURCES
	hdi08Adapter.h
	hdi08Adapter.cpp
)

# ----------------- UART0
#
# UART0 is the MCF5307 DUART module at MBAR+0x1C0 with vector 0x42 and the
# observed divider 0x36, 8N1. UART1 (MBAR+0x200) is unused and reads back its
# reset values; the same model owns both blocks. The transmitter buffer is the
# source for readMidiOut in the Device subclass. This model stores the divider
# as data and names no clock rate.

list(APPEND G2LIB_SOURCES
	uart0.h
	uart0.cpp
)

# ----------------- the P-memory write funnel
#
# Both files are listed: the header for the IDE source group, and the
# translation unit because a build that compiles the test without compiling this
# source fails at the link step on g2::writePMem.
#
# `.github/workflows/track-board.yml` fails when any other file under
# source/nord/g2/ names a P-memory write, so moving either name out of this
# directory moves the allow-list with it.

list(APPEND G2LIB_SOURCES
	pmemFunnel.h
	pmemFunnel.cpp
)

# ----------------- the M-Bus controller and the MAX1039 slave
#
# Both translation units are listed: a build that compiles the test without
# compiling them fails at the link step on g2::MBus and g2::Max1039.

list(APPEND G2LIB_SOURCES
	mbus.h
	mbus.cpp
	max1039.h
	max1039.cpp
)

# ----------------- the MCF5307 general-purpose timers
#
# One Timer is one of the two general-purpose timer modules the MCF5307
# carries, at MBAR+$140 and MBAR+$180. The Sim owns both and routes their
# register addresses to them; the Board advances them from the cycles runMcu
# actually ran.

list(APPEND G2LIB_SOURCES
	timer.h
	timer.cpp
)
