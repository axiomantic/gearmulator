# Source list for the chain track. Owned by the chain track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree. See plan section 7.4.2.

# ----------------- CHN-3, SlotWriteView
#
# Header-only on purpose: CHN-3's Files: line names slotWriteView.h and no
# translation unit. Listed here for the IDE source group, like anomalyLog.h.

list(APPEND G2LIB_SOURCES slotWriteView.h)


# ----------------- CHN-1, the mailbox delay line
#
# The whole allocation of the ring happens once, in the constructor, so the
# compiled part is a single translation unit.
list(APPEND G2LIB_SOURCES mailbox.cpp mailbox.h)

# ----------------- CHN-4/CHN-5, ChainTopology + the ChainAdapter class
#
# Both stay in the same source group so the IDE shows them together.
list(APPEND G2LIB_SOURCES chainAdapter.h chainAdapter.cpp)

# ----------------- the chain order, read out of the booted machine
#
# Which hardware port carries which chain position. The firmware chooses it and
# it is not the identity, so nothing may assume it; chainOrder.h carries the
# derivation and why it cannot be answered before the firmware has run.
list(APPEND G2LIB_SOURCES chainOrder.h chainOrder.cpp)
