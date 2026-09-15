cmake_minimum_required(VERSION 3.15)

add_subdirectory(g2Lib)
add_subdirectory(g2TestConsole)

# g2JucePlugin is deliberately not added as a subdirectory here. On this band the
# directory carries only translation units that g2Lib's own test targets compile
# by path; the plugin target itself arrives with the rest of its sources on the
# band that introduces it, and that band adds the subdirectory. Wiring it here
# would define a target nothing links. Guarding the add_subdirectory with an
# EXISTS test was rejected: it configures green while dropping the plugin, so a
# band that is supposed to build it would lose it without a diagnostic.
