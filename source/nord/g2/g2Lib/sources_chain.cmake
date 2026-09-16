# Source list for the chain track. Owned by the chain track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree.

# ----------------- SlotWriteView
#
# Header-only. Listed here for the IDE source group, like anomalyLog.h.

list(APPEND G2LIB_SOURCES slotWriteView.h)


# ----------------- the mailbox delay line
#
# The whole allocation of the ring happens once, in the constructor, so the
# compiled part is a single translation unit.
list(APPEND G2LIB_SOURCES mailbox.cpp mailbox.h)
