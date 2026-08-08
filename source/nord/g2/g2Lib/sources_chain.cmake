# Source list for the chain track. Owned by the chain track.
#
# Append every source name this track adds under source/nord/g2/g2Lib/ to
# G2LIB_SOURCES, with a path relative to this directory. Edit no other CMake
# file in this tree. See plan section 7.4.2.
#
# Created empty by task BRD-0.

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

