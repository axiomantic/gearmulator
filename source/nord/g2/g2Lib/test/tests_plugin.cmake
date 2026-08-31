# Test registrations for the plugin track. Owned by the plugin track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/nord/g2/g2Lib/test/. THE NAME IS THE EXACT STRING THE TASK'S Check:
# LINE PASSES TO -R. Edit no other CMake file in this tree.

# ----------------- g2TestConsole's subcommand surface
#
# Tier T0. Every child it spawns runs with NMG2_ARTIFACTS unset, so it boots no
# firmware and needs no artifact.
#
# It reads two things and holds no roster: the subcommand block `--help` prints,
# and the `command == "--name"` comparisons in main.cpp. The rule is that the
# two sets are equal, so the source path is a compile definition rather than a
# copied list.
#
# g2TestConsole lives in a sibling directory that is added after this one, so
# the path arrives as a generator expression and the build order as an explicit
# dependency. Without the dependency the fixture that builds this directory's
# executables would leave the console binary from the previous generation in
# place, and the test would read a stale surface.

# Registered only when g2TestConsole is part of this configure, and the
# condition is a question about the build and not about the source tree.
# t0_clock_guard configures a scratch project whose whole content is one
# add_subdirectory of g2Lib, so this file is read there too -- and there
# g2TestConsole is never added and $<TARGET_FILE:g2TestConsole> fails the
# generate step, taking that unrelated test red. `EXISTS` on the sibling path
# cannot discriminate the two: it is the same source tree in both. What
# discriminates them is g2Lib's parent directory: source/nord/g2 in the real
# build, which adds g2TestConsole beside g2Lib, and t0_clock_guard's scratch
# directory otherwise.
get_directory_property(g2_parentOfG2Lib DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." PARENT_DIRECTORY)

if(EXISTS "${g2_parentOfG2Lib}/g2TestConsole/CMakeLists.txt")
	add_executable(t0_console_subcommands t0_console_subcommands.cpp)
	set_property(TARGET t0_console_subcommands PROPERTY FOLDER "G2/test")
	target_compile_definitions(t0_console_subcommands PRIVATE
		G2_TEST_CONSOLE_EXECUTABLE="$<TARGET_FILE:g2TestConsole>"
		G2_TEST_CONSOLE_SOURCE="${g2_parentOfG2Lib}/g2TestConsole/main.cpp")
	add_dependencies(t0_console_subcommands g2TestConsole)

	add_test(NAME t0_console_subcommands COMMAND t0_console_subcommands)
	set_tests_properties(t0_console_subcommands PROPERTIES LABELS "UnitTest")

	# ----------------- `--impulse` reports an outcome word
	#
	# Tier T0. Its child runs with NMG2_ARTIFACTS unset, so it boots no firmware.
	#
	# It holds that a machine which never ran, a chain that carried nothing, and
	# an observer that saw nothing must not print the same thing.
	add_executable(t0_impulse_outcome t0_impulse_outcome.cpp)
	set_property(TARGET t0_impulse_outcome PROPERTY FOLDER "G2/test")
	target_compile_definitions(t0_impulse_outcome PRIVATE
		G2_TEST_CONSOLE_EXECUTABLE="$<TARGET_FILE:g2TestConsole>")
	target_include_directories(t0_impulse_outcome PRIVATE "${g2_parentOfG2Lib}")
	add_dependencies(t0_impulse_outcome g2TestConsole)

	add_test(NAME t0_impulse_outcome COMMAND t0_impulse_outcome)
	set_tests_properties(t0_impulse_outcome PROPERTIES LABELS "UnitTest")

	# ----------------- `--impulse` waits for the audio path, not for the loader
	#
	# Gated: the child boots the real firmware, so the test resolves
	# NMG2_ARTIFACTS through ArtifactResolver and skips when it is absent.
	#
	# The gate variables are computed here rather than borrowed from
	# tests_int.cmake, which CMakeLists.txt includes after this file, so the
	# variables do not exist yet at this point. The skip code is read out of
	# gatedFixture.h by the same regex tests_int.cmake and tests_board.cmake
	# use, so the spellings cannot drift; NMG2_ARTIFACTS is a cache variable, so
	# whichever include site sets it first wins.
	#
	# TIMEOUT 600 because the receive path does not arm until boot iteration
	# 231,296.

	add_executable(t1_rx_armed t1_rx_armed.cpp)
	target_link_libraries(t1_rx_armed PRIVATE g2Lib)
	set_property(TARGET t1_rx_armed PROPERTY FOLDER "G2/test")
	target_compile_definitions(t1_rx_armed PRIVATE
		G2_TEST_CONSOLE_EXECUTABLE="$<TARGET_FILE:g2TestConsole>")
	add_dependencies(t1_rx_armed g2TestConsole)

	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h")

	file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h" g2_rxArmedSkipExitCodeLine REGEX "g_gatedSkipExitCode = [0-9]+")

	if(NOT g2_rxArmedSkipExitCodeLine MATCHES "g_gatedSkipExitCode = ([0-9]+)")
		message(FATAL_ERROR "gatedFixture.h defines no g_gatedSkipExitCode, so ctest cannot be told which exit code is a skip")
	endif()

	set(g2_rxArmedSkipExitCode "${CMAKE_MATCH_1}")

	if(DEFINED ENV{NMG2_ARTIFACTS})
		set(g2_rxArmedArtifactsDefault "$ENV{NMG2_ARTIFACTS}")
	else()
		get_filename_component(g2_rxArmedArtifactsDefault "${CMAKE_SOURCE_DIR}/../nmg2-artifacts" ABSOLUTE)
	endif()

	set(NMG2_ARTIFACTS "${g2_rxArmedArtifactsDefault}" CACHE PATH "Directory holding the Clavia-derived G2 artifacts. Gated tests skip when it names no directory.")

	add_test(NAME t1_rx_armed COMMAND t1_rx_armed)
	set_tests_properties(t1_rx_armed PROPERTIES LABELS "IntegrationTest" TIMEOUT 600 SKIP_RETURN_CODE ${g2_rxArmedSkipExitCode})

	if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
		set_property(TEST t1_rx_armed APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
	endif()
else()
	message(STATUS "g2TestConsole is not part of this configure; t0_console_subcommands and t0_impulse_outcome are not registered")
endif()


# ----------------- PLG-1, the synthLib::Device subclass surface (PLG-3
#                   absorbed as STEP 2, plan §24.6 row W3-390)
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_device_surface$
#
# THE TWO TARGETS BELOW ARE DECLARED UNCONDITIONALLY AND OUTSIDE THE
# g2TestConsole GUARD ABOVE, which is the discipline the t0_mcf5307_link
# block in tests_board.cmake states: a registration that builds nothing when
# a guard is off would report a green by building nothing. These two do not
# touch g2TestConsole, so they have no reason to sit inside the guard.
#
# TIER T0 AND UNGATED. The test needs no firmware artifact: it constructs the
# device over an empty DeviceCreateParams and reads the surface.
#
# WHAT THE TEST HOLDS. One static_assert per each of the twelve pure virtuals
# synthLib::Device declares (as member-pointer bindings, which force the
# compiler rather than the linker), m_numSamplesProcessed as the subclass's
# OWN member, getSamplerate() == 96000.0f from a constructed device, and the
# SYNTHLIB_DEMO_MODE conditional shape asserted against the build.
#
# IT COMPILES ../../g2JucePlugin/g2Device.cpp DIRECTLY AND LINKS g2Lib.
# g2Device.cpp is a g2JucePlugin source and not a g2Lib source -- exactly the
# g2PatchLoad.cpp arrangement t0_gdb_script and t0_pch2_load already use.
# Naming the g2JucePlugin target here would couple this test to that
# directory's whole JUCE surface; the target build is the block's own second
# conjunct and does not run through this test.

add_executable(t0_device_surface
	t0_device_surface.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2State.cpp)
target_link_libraries(t0_device_surface PRIVATE g2Lib)
set_property(TARGET t0_device_surface PROPERTY FOLDER "G2/test")

add_test(NAME t0_device_surface COMMAND t0_device_surface)
set_tests_properties(t0_device_surface PROPERTIES LABELS "UnitTest")

# ----------------- PLG-1 step 2 (absorbed PLG-3), the two hand-off flags
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_handoff_flags$
#
# TIER T0 AND UNGATED: the device is constructed, never booted, and no
# firmware artifact is read.
#
# THE SEQ_CST PAIRING IS WHAT THIS TEST EXISTS TO FAIL. Four of the eight
# atomic operations of the hand-off are memory_order_seq_cst because each
# thread stores one flag and then loads the OTHER; weaken any one of the four
# and the two threads can each observe the other's pre-store value and both
# proceed onto the Scheduler. The hammer runs the two halves against each
# other and counts that outcome; the audio half is driven through the REAL
# processAudio. Threads is the audio thread, which lives on its own.

add_executable(t0_handoff_flags
	t0_handoff_flags.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2State.cpp)
target_link_libraries(t0_handoff_flags PRIVATE g2Lib)
find_package(Threads REQUIRED)
target_link_libraries(t0_handoff_flags PRIVATE Threads::Threads)
set_property(TARGET t0_handoff_flags PROPERTY FOLDER "G2/test")

add_test(NAME t0_handoff_flags COMMAND t0_handoff_flags)

# ----------------- PLG-7, readMidiOut
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_midi_out$
#
# TIER T0 AND UNGATED: no firmware artifact and no booted machine. The device
# is constructed over an empty DeviceCreateParams, exactly as t0_device_surface
# does, and the UART0 model is driven standalone, exactly as t0_uart0 does.
#
# WHAT THE TEST HOLDS. Design section 14.5 and section 17 row 7.30:
# readMidiOut carries ONLY what the machine originated -- no unsolicited
# SysEx, nothing periodic, no keepalive. Case group 1 holds the no-machine
# half (the PLG-1 state, where no Board exists yet); case group 2 drives the
# UART0 model's TX side through the SAME setMidiOut call PLG-12 performs on
# Board::uart0() and asserts readMidiOut carries exactly the originated
# bytes, in order, with the Device source, and not the byte a disabled
# transmitter withholds; case group 3 holds the no-unsolicited-SysEx half.
# The required-red mutation plants a keepalive (a periodic event pushed into
# the parser before every drain) and turns case group 1 red.
#
# IT COMPILES ../../g2JucePlugin/g2Device.cpp DIRECTLY AND LINKS g2Lib,
# the same arrangement as the two PLG-1 registrations above.

add_executable(t0_midi_out
	t0_midi_out.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2State.cpp)
target_link_libraries(t0_midi_out PRIVATE g2Lib)
set_property(TARGET t0_midi_out PROPERTY FOLDER "G2/test")

add_test(NAME t0_midi_out COMMAND t0_midi_out)
set_tests_properties(t0_midi_out PROPERTIES LABELS "UnitTest")

# ----------------- PLG-2, the channel counts
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_channel_counts$
#
# TIER T0 AND UNGATED: the device is constructed, never booted, and reads no
# artifact file. Case group 1 clears NMG2_ARTIFACTS IN-PROCESS (the resolver's
# empty-behaves-as-unset contract), so the invoking shell's value cannot
# decide the no-firmware clause; case group 2 points the variable at a
# temporary directory the test creates -- the constructor resolves firmware
# STATE (BRD-10) and does not boot, so no artifact file is needed.
#
# WHAT THE TEST HOLDS. Design sections 14.6, 14.7 and 17 rows 7.32/7.33:
# getChannelCountIn() and getChannelCountOut() return 2 and 2, final the
# instant the Device constructor returns -- before the firmware is loaded,
# before the boot, whether or not any artifact was found. The Plugin queries
# the counts exactly once (plugin.cpp:15, its member-initializer list) and
# ResamplerInOut stores the pair as const members, so the no-firmware path is
# the load-bearing clause. Each group asserts the FirmwareStatus state it
# walked, so neither can pass by accident with the other outcome in view.
#
# IT COMPILES ../../g2JucePlugin/g2Device.cpp DIRECTLY AND LINKS g2Lib, the
# same arrangement as the PLG-1 registrations above.

add_executable(t0_channel_counts
	t0_channel_counts.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2State.cpp)
target_link_libraries(t0_channel_counts PRIVATE g2Lib)
set_property(TARGET t0_channel_counts PROPERTY FOLDER "G2/test")

add_test(NAME t0_channel_counts COMMAND t0_channel_counts)
set_tests_properties(t0_channel_counts PROPERTIES LABELS "UnitTest")

# ----------------- PLG-5, getState and setState: the seven-item state
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_plugin_state$
#
# TIER T0 AND UNGATED: no firmware artifact is read (the Device constructor
# resolves firmware STATE, not content) and no machine is booted.
#
# WHAT THE TEST HOLDS. Design sections 15.5, 14.7, 17 row 7.29 and 15.8:
# the seven-item state format round-trips through the PLUGIN layer, not the
# Device alone -- the Plugin layer prepends its two-byte version header
# (plugin.cpp pushes g_stateVersion and the StateType) and design row 7.29's
# defect is a getState that assigns over it. The harness replicates the
# Plugin's exact contract over the real g2::Device (the same
# g2Device.cpp-direct arrangement as the PLG-1 registrations above) and adds
# ../../g2JucePlugin/g2State.cpp, the file PLG-5 creates. The required-red
# mutation is the planted assign() in the test-local ClobberingDevice: under
# the plugin contract the round trip goes red, because the image then begins
# with the device's own magic where the framework's version check demands
# g_stateVersion, and plugin.cpp's routing refuses it.

add_executable(t0_plugin_state
	t0_plugin_state.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2State.cpp)
target_link_libraries(t0_plugin_state PRIVATE g2Lib)
set_property(TARGET t0_plugin_state PROPERTY FOLDER "G2/test")

add_test(NAME t0_plugin_state COMMAND t0_plugin_state)
set_tests_properties(t0_plugin_state PROPERTIES LABELS "UnitTest")

# ----------------- PLG-4, processAudio (PLG-4 step 1; plan section 17)
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_process_audio$
#
# TIER T0 AND UNGATED: the device is constructed over an empty
# DeviceCreateParams and never booted; no firmware artifact is read.
#
# WHAT THE TEST HOLDS. Design sections 14.7, 13.6 and 13.10 rule 3: the
# audio callback's choreography. The not-ready path zeroes its buffers and
# touches the Scheduler not at all; the ready branch drives the Scheduler
# push -> runFrames -> pull -> faulted IN ORDER -- the order that fixes both
# codec queue capacities at L + B -- and answers a fault with the one
# response of design section 13.10.3 step 4 (a release store of false into
# m_ready). The order is asserted through a recorder installed through the
# Device's own Scheduler seam, because Scheduler's methods are not virtual
# and the property under test is the Device's choreography, not the
# Scheduler's.
#
# IT COMPILES ../../g2JucePlugin/g2Device.cpp DIRECTLY AND LINKS g2Lib, the
# same arrangement as the PLG-1 registrations above.

add_executable(t0_process_audio
	t0_process_audio.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2State.cpp)
target_link_libraries(t0_process_audio PRIVATE g2Lib)
find_package(Threads REQUIRED)
target_link_libraries(t0_process_audio PRIVATE Threads::Threads)
set_property(TARGET t0_process_audio PROPERTY FOLDER "G2/test")

add_test(NAME t0_process_audio COMMAND t0_process_audio)
set_tests_properties(t0_process_audio PROPERTIES LABELS "UnitTest")

# ----------------- PLG-4 step 2 (absorbed PLG-6), sendMidi and the offset
#                   conversion
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_midi_offsets$
#
# TIER T0 AND UNGATED: no firmware artifact, no booted machine; the counter
# the conversion reads is advanced by driving the REAL processAudio's
# not-ready tail.
#
# WHAT THE TEST HOLDS. Design sections 17 rows 7.30, 7.31 and 7.34: the
# conversion is block-relative to ABSOLUTE
# (offset += m_numSamplesProcessed + getExtraLatencySamples()), not the
# reverse; the counter is the subclass's OWN member, so the acceptance
# criterion of review finding I18 holds -- the test compiles against a
# synthLib::Device subclass with no wLib dependency. The no-double-delivery
# row: a reply to a host-sent message never appears in readMidiOut.
#
# IT COMPILES ../../g2JucePlugin/g2Device.cpp DIRECTLY AND LINKS g2Lib.

add_executable(t0_midi_offsets
	t0_midi_offsets.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2State.cpp)
target_link_libraries(t0_midi_offsets PRIVATE g2Lib)
set_property(TARGET t0_midi_offsets PROPERTY FOLDER "G2/test")

add_test(NAME t0_midi_offsets COMMAND t0_midi_offsets)
set_tests_properties(t0_midi_offsets PROPERTIES LABELS "UnitTest")

# ----------------- PLG-8, the resampler mode
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_resampler_mode$
#
# TIER T0 AND UNGATED: the device is constructed over an empty
# DeviceCreateParams and never booted; no firmware artifact is read.
#
# WHAT THE TEST HOLDS. Design sections 14.2.2, 18.2 and 24: the plugin
# SETS the resampler mode -- synthLib::Plugin::setResamplerMode with
# Mode::MameHq -- during construction, before the first prepareToPlay, and
# never inherits the framework default Legacy, which is the default in two
# places (resampler.h:30, resamplerInOut.h:43). The constructed plugin
# reports MameHq and the framework's own observable agrees: the test drives
# setHostSamplerate, the framework's prepareToPlay analogue, which runs
# ResamplerInOut::recreate() and its 512-sample pre-warm with an EMPTY
# process callback -- the Device is never invoked -- and the latency the
# Plugin then reports differs measurably between Legacy and MameHq. The
# required-red control is a subclass that skips the set: it stands at the
# framework defaults and leaves the Legacy figure.
#
# IT COMPILES ../../g2JucePlugin/g2Plugin.cpp DIRECTLY alongside
# g2Device.cpp and g2State.cpp and links g2Lib, the same arrangement as the
# other plugin-track registrations. g2Plugin.cpp is a g2JucePlugin source
# and not a g2Lib source; naming the g2JucePlugin target here would couple
# this test to that directory's whole surface, exactly as the PLG-1
# registration above records for g2Device.cpp.

add_executable(t0_resampler_mode
	t0_resampler_mode.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2Plugin.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2State.cpp)
target_link_libraries(t0_resampler_mode PRIVATE g2Lib)
set_property(TARGET t0_resampler_mode PROPERTY FOLDER "G2/test")

add_test(NAME t0_resampler_mode COMMAND t0_resampler_mode)
set_tests_properties(t0_resampler_mode PROPERTIES LABELS "UnitTest")

# ----------------- PLG-11, the passband ripple sweep
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_resampler_passband_ripple$
#
# TIER T0 AND UNGATED: no firmware artifact, no booted machine, no Device and
# no Plugin. The test constructs synthLib::ResamplerInOut directly, which is
# what PLG-11 requires -- "through ResamplerInOut alone".
#
# WHAT THE TEST HOLDS. Design sections 14.2.2 and 18.2: a stepped sine sweep
# from 20 Hz to 20 kHz at each of the six host rates of section 14.1's table,
# against a 96 kHz device rate, in the Mode::MameHq PLG-8 adopted. The
# reported figure is the peak-to-peak deviation from 0 dB and it is compared
# against a COMMITTED CONSTANT in the test source -- g_committedTargetDb. No
# code path in the test writes that target, and the test opens no file and
# reads no environment variable at all, which is the property that
# distinguishes this row from the one PLG-11 replaced: a check that rewrites
# its own target passes for every possible measurement.
#
# EVERY TONE IS COHERENT WITH ITS OWN CAPTURE WINDOW, so the analysis is a
# leak-free rectangular-window DFT bin rather than a windowed estimate with an
# error budget. The analyzer carries its own known positive and known negative
# on synthesized tones, so a blind analyzer cannot be mistaken for a flat
# filter, and the permanent control sweeps the same band through the framework
# default Mode::Legacy, whose 44.1 kHz passband edge sits at 19,845 Hz --
# below the 20 kHz the target is stated over -- and must exceed the target.
#
# IT LINKS g2Lib AND COMPILES NO g2JucePlugin SOURCE. Unlike the other
# plugin-track registrations above, this test needs neither g2::Device nor
# g2::Plugin: the object under test belongs to synthLib, which g2Lib already
# carries. Adding g2Device.cpp here would couple the check to a device the
# sweep must not construct.

add_executable(t0_resampler_passband_ripple t0_resampler_passband_ripple.cpp)
target_link_libraries(t0_resampler_passband_ripple PRIVATE g2Lib)
set_property(TARGET t0_resampler_passband_ripple PROPERTY FOLDER "G2/test")

add_test(NAME t0_resampler_passband_ripple COMMAND t0_resampler_passband_ripple)
set_tests_properties(t0_resampler_passband_ripple PROPERTIES LABELS "UnitTest")

# ----------------- PLG-12, the boot-on-restore sequence
#
# Check: ctest --test-dir build --no-tests=error -R ^t1_boot_on_restore$
#
# TIER T1 AND GATED. The test boots the REAL Clavia firmware through
# g2::Device::boot, so it resolves NMG2_ARTIFACTS through ArtifactResolver and
# reports design section 18.5's skip line when it is absent. Its two CONTROLS
# are ungated and run even on a skipped machine: they construct their own Board
# and Scheduler and need no artifact, which is what keeps a skipped run from
# also losing the evidence that the test's predicates discriminate.
#
# WHAT THE TEST HOLDS. Design section 15.6's six-step order, through the
# Device::IBootObserver seam PLG-12 adds -- the boot thread's twin of PLG-4's
# ISchedulerDriver: (A) the cold boot's five steps in order and no stateLoad;
# (B) step 2 leaves both codec queues EMPTY -- reset does not prime, and priming
# the sink with L frames is step 5's job; (C) step 4 runs the BOOT codec regime,
# so all four codec counters stand at zero afterwards and the boot cannot stall
# on a full sink; (D) the restoring boot runs stateLoad AFTER reset and BEFORE
# the boot quanta. The two planted controls are a REORDERED sequence, which the
# order predicate must refuse, and a PRIMED sink reached through beginPlayPhase,
# which the not-priming predicate must report as primed.
#
# IT COMPILES ../../g2JucePlugin/g2Device.cpp DIRECTLY AND LINKS g2Lib, the
# same arrangement every other plugin-track registration above uses.
#
# THE GATE VARIABLES ARE COMPUTED HERE rather than borrowed from the t1_rx_armed
# block above, which sits inside this file's g2TestConsole guard: a configure in
# which that guard is off would leave them undefined. NMG2_ARTIFACTS is a cache
# variable, so whichever site sets it first wins and the two cannot disagree.
# The skip code is READ OUT OF gatedFixture.h by the same regex the other sites
# use, so the spellings cannot drift.
#
# TIMEOUT 600, AND THE FIGURE IT GUARDS IS THE BUDGET AND NOT THE MEASUREMENT.
# The boot's ceiling is 500,000 emulated frames, which is above the roughly
# 425,000 t1_boot and g2TestConsole both measure to the patch browser. A healthy
# run spends far less: the boot leaves EARLY on Scheduler::chainAttached(), the
# machine's own signal that the DSP programs have landed, and that arrives at
# roughly 17,000 frames -- MEASURED at 16,960 on this host, whole run 8 seconds.
# The timeout is sized for the ceiling a firmware that never gets there would
# spend, not for the measurement.

add_executable(t1_boot_on_restore
	t1_boot_on_restore.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2State.cpp)
target_link_libraries(t1_boot_on_restore PRIVATE g2Lib)
set_property(TARGET t1_boot_on_restore PROPERTY FOLDER "G2/test")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h")

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h" g2_bootOnRestoreSkipExitCodeLine REGEX "g_gatedSkipExitCode = [0-9]+")

if(NOT g2_bootOnRestoreSkipExitCodeLine MATCHES "g_gatedSkipExitCode = ([0-9]+)")
	message(FATAL_ERROR "gatedFixture.h defines no g_gatedSkipExitCode, so ctest cannot be told which exit code is a skip")
endif()

set(g2_bootOnRestoreSkipExitCode "${CMAKE_MATCH_1}")

if(DEFINED ENV{NMG2_ARTIFACTS})
	set(g2_bootOnRestoreArtifactsDefault "$ENV{NMG2_ARTIFACTS}")
else()
	get_filename_component(g2_bootOnRestoreArtifactsDefault "${CMAKE_SOURCE_DIR}/../nmg2-artifacts" ABSOLUTE)
endif()

set(NMG2_ARTIFACTS "${g2_bootOnRestoreArtifactsDefault}" CACHE PATH "Directory holding the Clavia-derived G2 artifacts. Gated tests skip when it names no directory.")

add_test(NAME t1_boot_on_restore COMMAND t1_boot_on_restore)
set_tests_properties(t1_boot_on_restore PROPERTIES LABELS "IntegrationTest" TIMEOUT 600 SKIP_RETURN_CODE ${g2_bootOnRestoreSkipExitCode})

if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
	set_property(TEST t1_boot_on_restore APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
endif()

# ----------------- `B`, the largest host block the plugin accepts
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_max_host_block$
#
# TIER T0 AND UNGATED: no firmware artifact and no booted machine. B is derived
# from the host's maximum block and the host's rate alone, and the B = 0
# rejection is a Config-only rejection inside Scheduler::create.
#
# IT COMPILES ../../g2JucePlugin/g2Plugin.cpp DIRECTLY alongside g2Device.cpp
# and g2State.cpp and links g2Lib, the same arrangement as the other
# plugin-track registrations: g2Plugin.cpp is a g2JucePlugin source and naming
# that directory's target here would couple this test to its whole surface.

add_executable(t0_max_host_block
	t0_max_host_block.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2Plugin.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2State.cpp)
target_link_libraries(t0_max_host_block PRIVATE g2Lib)
set_property(TARGET t0_max_host_block PROPERTY FOLDER "G2/test")

add_test(NAME t0_max_host_block COMMAND t0_max_host_block)
set_tests_properties(t0_max_host_block PROPERTIES LABELS "UnitTest")

# ----------------- the latency the plugin reports to the host
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_latency_formula$
#
# TIER T0 AND UNGATED, for the same reason: the formula reads three constants,
# the chain configuration and two framework getters, and boots nothing.
#
# THE ASSERTION THIS REGISTRATION EXISTS FOR is the CEILING. Above 16,384
# frames the framework clamps the latency it is handed and only logs it, so the
# plugin would report a figure shorter than it takes and every host would
# silently mis-compensate. The test asserts the sum against that bound both
# arithmetically and behaviorally, through Scheduler::create's acceptance at
# the bound and its BadLookahead one frame above it.

add_executable(t0_latency_formula
	t0_latency_formula.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2Plugin.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../g2JucePlugin/g2State.cpp)
target_link_libraries(t0_latency_formula PRIVATE g2Lib)
set_property(TARGET t0_latency_formula PROPERTY FOLDER "G2/test")

add_test(NAME t0_latency_formula COMMAND t0_latency_formula)
set_tests_properties(t0_latency_formula PROPERTIES LABELS "UnitTest")
