# Test registrations for the plugin track. Owned by the plugin track.
#
# Append one add_test(NAME <name> ...) for every test this track adds under
# source/claudia/rg2/rg2Lib/test/. The NAME is the exact string passed to -R. Edit no
# other CMake file in this tree.

# ----------------- rg2TestConsole's subcommand surface
#
# Tier T0. Every child it spawns runs with NMG2_ARTIFACTS unset, so it boots no
# firmware and needs no artifact.
#
# It reads two things and holds no roster: the subcommand block `--help` prints,
# and the `command == "--name"` comparisons in main.cpp. The rule is that the
# two sets are equal, so the source path is a compile definition rather than a
# copied list.
#
# rg2TestConsole lives in a sibling directory that is added after this one, so
# the path arrives as a generator expression and the build order as an explicit
# dependency. Without the dependency a build of this directory's executables
# would leave the console binary from the previous generation in place, and the
# test would read a stale surface.

# Registered only when rg2TestConsole is part of this configure, and the
# condition is a question about the build and not about the source tree.
# `EXISTS` on the sibling path cannot answer it: a tree that adds rg2Lib without
# adding rg2TestConsole beside it has the same sources. What discriminates them
# is rg2Lib's parent directory: source/claudia/rg2 in a build of this repository,
# which adds rg2TestConsole beside rg2Lib. Where rg2Lib is added from anywhere
# else, $<TARGET_FILE:rg2TestConsole> would fail the generate step.
get_directory_property(g2_parentOfG2Lib DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.." PARENT_DIRECTORY)

if(EXISTS "${g2_parentOfG2Lib}/rg2TestConsole/CMakeLists.txt")
	add_executable(t0_console_subcommands t0_console_subcommands.cpp)
	set_property(TARGET t0_console_subcommands PROPERTY FOLDER "RG2/test")
	target_compile_definitions(t0_console_subcommands PRIVATE
		G2_TEST_CONSOLE_EXECUTABLE="$<TARGET_FILE:rg2TestConsole>"
		G2_TEST_CONSOLE_SOURCE="${g2_parentOfG2Lib}/rg2TestConsole/main.cpp")
	add_dependencies(t0_console_subcommands rg2TestConsole)

	add_test(NAME t0_console_subcommands COMMAND t0_console_subcommands)
	set_tests_properties(t0_console_subcommands PROPERTIES LABELS "UnitTest")

	# ----------------- `--impulse` reports an outcome word
	#
	# Tier T0. Its child runs with NMG2_ARTIFACTS unset, so it boots no firmware.
	#
	# It holds that a machine which never ran, a chain that carried nothing, and
	# an observer that saw nothing must not print the same thing.
	add_executable(t0_impulse_outcome t0_impulse_outcome.cpp)
	set_property(TARGET t0_impulse_outcome PROPERTY FOLDER "RG2/test")
	target_compile_definitions(t0_impulse_outcome PRIVATE
		G2_TEST_CONSOLE_EXECUTABLE="$<TARGET_FILE:rg2TestConsole>")
	target_include_directories(t0_impulse_outcome PRIVATE "${g2_parentOfG2Lib}")
	add_dependencies(t0_impulse_outcome rg2TestConsole)

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
	target_link_libraries(t1_rx_armed PRIVATE rg2Lib)
	set_property(TARGET t1_rx_armed PROPERTY FOLDER "RG2/test")
	target_compile_definitions(t1_rx_armed PRIVATE
		G2_TEST_CONSOLE_EXECUTABLE="$<TARGET_FILE:rg2TestConsole>")
	add_dependencies(t1_rx_armed rg2TestConsole)

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
	message(STATUS "rg2TestConsole is not part of this configure; t0_console_subcommands and t0_impulse_outcome are not registered")
endif()


# ----------------- the synthLib::Device subclass surface
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_device_surface$
#
# The two targets below are declared unconditionally and outside the
# rg2TestConsole guard above: a registration that builds nothing when a guard is
# off would report a green by building nothing. These two do not touch
# rg2TestConsole, so they have no reason to sit inside the guard.
#
# Tier T0 and ungated. The test needs no firmware artifact: it constructs the
# device over an empty DeviceCreateParams and reads the surface. The pure
# virtuals are bound as member pointers, which forces the compiler rather than
# the linker.
#
# It compiles ../../rg2JucePlugin/rg2Device.cpp directly and links rg2Lib.
# rg2Device.cpp is a rg2JucePlugin source and not a rg2Lib source; naming the
# rg2JucePlugin target here would couple this test to that directory's whole
# JUCE surface. This is the standing arrangement for the plugin-track
# registrations below, which name only what they add to it.

add_executable(t0_device_surface
	t0_device_surface.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2State.cpp)
target_link_libraries(t0_device_surface PRIVATE rg2Lib)
set_property(TARGET t0_device_surface PROPERTY FOLDER "RG2/test")

add_test(NAME t0_device_surface COMMAND t0_device_surface)
set_tests_properties(t0_device_surface PROPERTIES LABELS "UnitTest")

# ----------------- the two hand-off flags
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_handoff_flags$
#
# Tier T0 and ungated: the device is constructed, never booted, and no firmware
# artifact is read.
#
# Four of the eight atomic operations of the hand-off are memory_order_seq_cst
# because each thread stores one flag and then loads the other; weaken any one
# of the four and the two threads can each observe the other's pre-store value
# and both proceed onto the Scheduler. The hammer runs the two halves against
# each other and counts that outcome; the audio half is driven through the real
# processAudio. Threads is the audio thread, which lives on its own.

add_executable(t0_handoff_flags
	t0_handoff_flags.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2State.cpp)
target_link_libraries(t0_handoff_flags PRIVATE rg2Lib)
find_package(Threads REQUIRED)
target_link_libraries(t0_handoff_flags PRIVATE Threads::Threads)
set_property(TARGET t0_handoff_flags PROPERTY FOLDER "RG2/test")

add_test(NAME t0_handoff_flags COMMAND t0_handoff_flags)
set_tests_properties(t0_handoff_flags PROPERTIES LABELS "UnitTest")

# ----------------- readMidiOut
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_midi_out$
#
# Tier T0 and ungated: no firmware artifact and no booted machine. The device is
# constructed over an empty DeviceCreateParams and the UART0 model is driven
# standalone.
#
# readMidiOut carries only what the machine originated -- no unsolicited SysEx,
# nothing periodic, no keepalive.
#
# It compiles ../../rg2JucePlugin/rg2Device.cpp directly and links rg2Lib.

add_executable(t0_midi_out
	t0_midi_out.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2State.cpp)
target_link_libraries(t0_midi_out PRIVATE rg2Lib)
set_property(TARGET t0_midi_out PROPERTY FOLDER "RG2/test")

add_test(NAME t0_midi_out COMMAND t0_midi_out)
set_tests_properties(t0_midi_out PROPERTIES LABELS "UnitTest")

# ----------------- the channel counts
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_channel_counts$
#
# Tier T0 and ungated: the device is constructed, never booted, and reads no
# artifact file. Case group 1 clears NMG2_ARTIFACTS in-process (the resolver's
# empty-behaves-as-unset contract), so the invoking shell's value cannot decide
# the no-firmware clause; case group 2 points the variable at a temporary
# directory the test creates -- the constructor resolves firmware state and does
# not boot, so no artifact file is needed.
#
# getChannelCountIn() and getChannelCountOut() are final the instant the Device
# constructor returns -- before the firmware is loaded, before the boot, whether
# or not any artifact was found. The Plugin queries the counts once
# (plugin.cpp:15, its member-initializer list) and ResamplerInOut stores the
# pair as const members, so the no-firmware path is the load-bearing clause.
# Each group asserts the FirmwareStatus state it walked, so neither can pass by
# accident with the other outcome in view.
#
# It compiles ../../rg2JucePlugin/rg2Device.cpp directly and links rg2Lib.

add_executable(t0_channel_counts
	t0_channel_counts.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2State.cpp)
target_link_libraries(t0_channel_counts PRIVATE rg2Lib)
set_property(TARGET t0_channel_counts PROPERTY FOLDER "RG2/test")

add_test(NAME t0_channel_counts COMMAND t0_channel_counts)
set_tests_properties(t0_channel_counts PROPERTIES LABELS "UnitTest")

# ----------------- getState and setState: the seven-item state
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_plugin_state$
#
# Tier T0 and ungated: no firmware artifact is read (the Device constructor
# resolves firmware state, not content) and no machine is booted.
#
# The seven-item state format round-trips through the plugin layer, not the
# Device alone: the Plugin layer prepends its two-byte version header
# (plugin.cpp pushes g_stateVersion and the StateType), and the defect guarded
# against is a getState that assigns over it. The harness replicates the
# Plugin's contract over the real rg2::Device and adds
# ../../rg2JucePlugin/rg2State.cpp.
#
# It compiles ../../rg2JucePlugin/rg2Device.cpp directly and links rg2Lib.

add_executable(t0_plugin_state
	t0_plugin_state.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2State.cpp)
target_link_libraries(t0_plugin_state PRIVATE rg2Lib)
set_property(TARGET t0_plugin_state PROPERTY FOLDER "RG2/test")

add_test(NAME t0_plugin_state COMMAND t0_plugin_state)
set_tests_properties(t0_plugin_state PROPERTIES LABELS "UnitTest")

# ----------------- processAudio
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_process_audio$
#
# Tier T0 and ungated: the device is constructed over an empty
# DeviceCreateParams and never booted; no firmware artifact is read.
#
# The audio callback's choreography. The not-ready path zeroes its buffers and
# touches the Scheduler not at all; the ready branch drives the Scheduler
# push -> runFrames -> pull -> faulted in order -- the order that fixes both
# codec queue capacities at L + B -- and answers a fault with a release store of
# false into m_ready. The order is asserted through a recorder installed through
# the Device's own Scheduler seam, because Scheduler's methods are not virtual
# and the property under test is the Device's choreography, not the Scheduler's.
#
# It compiles ../../rg2JucePlugin/rg2Device.cpp directly and links rg2Lib.

add_executable(t0_process_audio
	t0_process_audio.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2State.cpp)
target_link_libraries(t0_process_audio PRIVATE rg2Lib)
find_package(Threads REQUIRED)
target_link_libraries(t0_process_audio PRIVATE Threads::Threads)
set_property(TARGET t0_process_audio PROPERTY FOLDER "RG2/test")

add_test(NAME t0_process_audio COMMAND t0_process_audio)
set_tests_properties(t0_process_audio PROPERTIES LABELS "UnitTest")

# ----------------- sendMidi and the offset conversion
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_midi_offsets$
#
# Tier T0 and ungated: no firmware artifact, no booted machine; the counter the
# conversion reads is advanced by driving the real processAudio's not-ready
# tail.
#
# The conversion is block-relative to absolute
# (offset += m_numSamplesProcessed + getExtraLatencySamples()), not the reverse;
# the counter is the subclass's own member, so the test compiles against a
# synthLib::Device subclass with no wLib dependency. A reply to a host-sent
# message never appears in readMidiOut.
#
# It compiles ../../rg2JucePlugin/rg2Device.cpp directly and links rg2Lib.

add_executable(t0_midi_offsets
	t0_midi_offsets.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2State.cpp)
target_link_libraries(t0_midi_offsets PRIVATE rg2Lib)
set_property(TARGET t0_midi_offsets PROPERTY FOLDER "RG2/test")

add_test(NAME t0_midi_offsets COMMAND t0_midi_offsets)
set_tests_properties(t0_midi_offsets PROPERTIES LABELS "UnitTest")

# ----------------- the resampler mode
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_resampler_mode$
#
# Tier T0 and ungated: the device is constructed over an empty
# DeviceCreateParams and never booted; no firmware artifact is read.
#
# The plugin sets the resampler mode -- synthLib::Plugin::setResamplerMode with
# Mode::MameHq -- during construction, before the first prepareToPlay, and never
# inherits the framework default Legacy (resampler.h:30, resamplerInOut.h:43).
# The test drives setHostSamplerate, the framework's prepareToPlay analogue,
# which runs ResamplerInOut::recreate() and its 512-sample pre-warm with an
# empty process callback -- the Device is never invoked -- and the latency the
# Plugin then reports differs measurably between Legacy and MameHq. The control
# is a subclass that skips the set and stands at the framework defaults.
#
# It compiles ../../rg2JucePlugin/rg2Plugin.cpp directly alongside rg2Device.cpp
# and rg2State.cpp and links rg2Lib.

add_executable(t0_resampler_mode
	t0_resampler_mode.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Plugin.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2State.cpp)
target_link_libraries(t0_resampler_mode PRIVATE rg2Lib)
set_property(TARGET t0_resampler_mode PROPERTY FOLDER "RG2/test")

add_test(NAME t0_resampler_mode COMMAND t0_resampler_mode)
set_tests_properties(t0_resampler_mode PROPERTIES LABELS "UnitTest")

# ----------------- the passband ripple sweep
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_resampler_passband_ripple$
#
# Tier T0 and ungated: no firmware artifact, no booted machine, no Device and no
# Plugin. The test constructs synthLib::ResamplerInOut alone.
#
# A stepped sine sweep from 20 Hz to 20 kHz at each supported host rate, against
# a 96 kHz device rate, in Mode::MameHq. The reported figure is the peak-to-peak
# deviation from 0 dB and it is compared against a committed constant in the
# test source, g_committedTargetDb. No code path in the test writes that target,
# and the test opens no file and reads no environment variable at all: a check
# that rewrites its own target passes for every possible measurement.
#
# Every tone is coherent with its own capture window, so the analysis is a
# leak-free rectangular-window DFT bin rather than a windowed estimate with an
# error budget. The analyzer carries its own known positive and known negative
# on synthesized tones, so a blind analyzer cannot be mistaken for a flat
# filter, and the permanent control sweeps the same band through the framework
# default Mode::Legacy, whose 44.1 kHz passband edge sits at 19,845 Hz -- below
# the 20 kHz the target is stated over -- and must exceed the target.
#
# It links rg2Lib and compiles no rg2JucePlugin source: the object under test
# belongs to synthLib, which rg2Lib already carries. Adding rg2Device.cpp here
# would couple the check to a device the sweep must not construct.

add_executable(t0_resampler_passband_ripple t0_resampler_passband_ripple.cpp)
target_link_libraries(t0_resampler_passband_ripple PRIVATE rg2Lib)
set_property(TARGET t0_resampler_passband_ripple PROPERTY FOLDER "RG2/test")

add_test(NAME t0_resampler_passband_ripple COMMAND t0_resampler_passband_ripple)
set_tests_properties(t0_resampler_passband_ripple PROPERTIES LABELS "UnitTest")

# ----------------- the boot-on-restore sequence
#
# Check: ctest --test-dir build --no-tests=error -R ^t1_boot_on_restore$
#
# Tier T1 and gated. The test boots the real Clavia firmware through
# rg2::Device::boot, so it resolves NMG2_ARTIFACTS through ArtifactResolver and
# reports the skip line when it is absent. Its two controls are ungated and run
# even on a skipped machine: they construct their own Board and Scheduler and
# need no artifact, which is what keeps a skipped run from also losing the
# evidence that the test's predicates discriminate.
#
# The six-step boot order, through the Device::IBootObserver seam -- the boot
# thread's twin of ISchedulerDriver: (A) the cold boot's five steps in order and
# no stateLoad; (B) step 2 leaves both codec queues empty -- reset does not
# prime, and priming the sink with L frames is step 5's job; (C) step 4 runs the
# boot codec regime, so the codec counters stand at zero afterwards and the boot
# cannot stall on a full sink; (D) the restoring boot runs stateLoad after reset
# and before the boot quanta. The two planted controls are a reordered sequence,
# which the order predicate must refuse, and a primed sink reached through
# beginPlayPhase, which the not-priming predicate must report as primed.
#
# It compiles ../../rg2JucePlugin/rg2Device.cpp directly and links rg2Lib.
#
# The gate variables are computed here rather than borrowed from the t1_rx_armed
# block above, which sits inside this file's rg2TestConsole guard: a configure in
# which that guard is off would leave them undefined. NMG2_ARTIFACTS is a cache
# variable, so whichever site sets it first wins and the two cannot disagree.
# The skip code is read out of gatedFixture.h by the same regex the other sites
# use, so the spellings cannot drift.
#
# TIMEOUT 600, and the figure it guards is the budget and not the measurement.
# The boot's ceiling is 500,000 emulated frames, above the roughly 425,000
# measured to the patch browser. A healthy run spends far less: the boot leaves
# early on Scheduler::chainAttached(), the machine's own signal that the DSP
# programs have landed, measured at 16,960 frames on this host, whole run 8
# seconds. The timeout is sized for the ceiling a firmware that never gets there
# would spend, not for the measurement.

add_executable(t1_boot_on_restore
	t1_boot_on_restore.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2State.cpp)
target_link_libraries(t1_boot_on_restore PRIVATE rg2Lib)
set_property(TARGET t1_boot_on_restore PROPERTY FOLDER "RG2/test")

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
# Tier T0 and ungated: no firmware artifact and no booted machine. B is derived
# from the host's maximum block and the host's rate alone, and the B = 0
# rejection is a Config-only rejection inside Scheduler::create.
#
# It compiles ../../rg2JucePlugin/rg2Plugin.cpp directly alongside rg2Device.cpp
# and rg2State.cpp and links rg2Lib.

add_executable(t0_max_host_block
	t0_max_host_block.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Plugin.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2State.cpp)
target_link_libraries(t0_max_host_block PRIVATE rg2Lib)
set_property(TARGET t0_max_host_block PROPERTY FOLDER "RG2/test")

add_test(NAME t0_max_host_block COMMAND t0_max_host_block)
set_tests_properties(t0_max_host_block PROPERTIES LABELS "UnitTest")

# ----------------- the latency the plugin reports to the host
#
# Check: ctest --test-dir build --no-tests=error -R ^t0_latency_formula$
#
# Tier T0 and ungated, for the same reason: the formula reads a few constants,
# the chain configuration and two framework getters, and boots nothing.
#
# The assertion this registration exists for is the ceiling. Above 16,384 frames
# the framework clamps the latency it is handed and only logs it, so the plugin
# would report a figure shorter than it takes and every host would silently
# mis-compensate. The test asserts the sum against that bound both
# arithmetically and behaviorally, through Scheduler::create's acceptance at the
# bound and its BadLookahead one frame above it.

add_executable(t0_latency_formula
	t0_latency_formula.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Plugin.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2State.cpp)
target_link_libraries(t0_latency_formula PRIVATE rg2Lib)
set_property(TARGET t0_latency_formula PROPERTY FOLDER "RG2/test")

add_test(NAME t0_latency_formula COMMAND t0_latency_formula)
set_tests_properties(t0_latency_formula PROPERTIES LABELS "UnitTest")
# ----------------- the state hand-off under load
#
# Check: ctest --test-dir build --no-tests=error -R ^t1_state_handoff_load$
#        on a thread-sanitizer build (-DG2_THREAD_SANITIZER=ON).
#
# Tier T1 and gated, through the same gatedFixture.h path every other gated
# registration in this tree uses.
#
# processAudio runs continuously on one thread while a second thread drives
# getState, setState and the beginStateChange / endStateChange window in a loop
# for 60 seconds. The assertion the run makes is that no data race is reported,
# plus the run's own known positives: the audio half reached the shared payload,
# the message half completed rounds, and the acknowledgement wait completed once
# per round.
#
# The negative case is the second target below, and it is what makes the first
# one's green mean something. It is the same source with G2_HANDOFF_DROP_ACK
# defined, which removes the beginStateChange call from the message half's
# payload edit -- the acknowledgement, and the store that withdraws readiness.
# The audio half then stays on the ready branch and touches the payload while
# the message half writes it. Its ctest pass condition is the sanitizer's own
# race report and not an exit code: a build that produced no report fails this
# test, which is how the pair shows the instrument can see the thing it is
# looking for.
#
# The sanitizer is opt-in and off by default. It is applied per target rather
# than to the whole tree, so rg2Lib is linked uninstrumented: a race entirely
# inside rg2Lib would not be seen. Both accesses under test are in this test's
# own translation unit and in rg2Device.cpp, which the target compiles directly,
# so both are instrumented.
#
# The negative target is registered only on a sanitizer build, because without
# the sanitizer it can never produce the report its pass condition names, and a
# test that cannot pass is a permanent red rather than a check.

option(G2_THREAD_SANITIZER "Build the plugin-track concurrency tests with -fsanitize=thread" OFF)

add_executable(t1_state_handoff_load
	t1_state_handoff_load.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2State.cpp)
target_link_libraries(t1_state_handoff_load PRIVATE rg2Lib)
find_package(Threads REQUIRED)
target_link_libraries(t1_state_handoff_load PRIVATE Threads::Threads)
set_property(TARGET t1_state_handoff_load PROPERTY FOLDER "RG2/test")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h")

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h" g2_handoffLoadSkipExitCodeLine REGEX "g_gatedSkipExitCode = [0-9]+")

if(NOT g2_handoffLoadSkipExitCodeLine MATCHES "g_gatedSkipExitCode = ([0-9]+)")
	message(FATAL_ERROR "gatedFixture.h defines no g_gatedSkipExitCode, so ctest cannot be told which exit code is a skip")
endif()

set(g2_handoffLoadSkipExitCode "${CMAKE_MATCH_1}")

if(DEFINED ENV{NMG2_ARTIFACTS})
	set(g2_handoffLoadArtifactsDefault "$ENV{NMG2_ARTIFACTS}")
else()
	get_filename_component(g2_handoffLoadArtifactsDefault "${CMAKE_SOURCE_DIR}/../nmg2-artifacts" ABSOLUTE)
endif()

set(NMG2_ARTIFACTS "${g2_handoffLoadArtifactsDefault}" CACHE PATH "Directory holding the Clavia-derived G2 artifacts. Gated tests skip when it names no directory.")

# TIMEOUT 900: the load window is 60 seconds and a thread-sanitizer build runs
# the same work several times slower.
add_test(NAME t1_state_handoff_load COMMAND t1_state_handoff_load)
set_tests_properties(t1_state_handoff_load PROPERTIES LABELS "IntegrationTest" TIMEOUT 900 SKIP_RETURN_CODE ${g2_handoffLoadSkipExitCode})

if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
	set_property(TEST t1_state_handoff_load APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
endif()

if(G2_THREAD_SANITIZER)
	target_compile_options(t1_state_handoff_load PRIVATE -fsanitize=thread -g)
	target_link_options(t1_state_handoff_load PRIVATE -fsanitize=thread)

	add_executable(t1_state_handoff_load_negative
		t1_state_handoff_load.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Device.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2State.cpp)
	target_link_libraries(t1_state_handoff_load_negative PRIVATE rg2Lib Threads::Threads)
	target_compile_definitions(t1_state_handoff_load_negative PRIVATE G2_HANDOFF_DROP_ACK=1)
	target_compile_options(t1_state_handoff_load_negative PRIVATE -fsanitize=thread -g)
	target_link_options(t1_state_handoff_load_negative PRIVATE -fsanitize=thread)
	set_property(TARGET t1_state_handoff_load_negative PROPERTY FOLDER "RG2/test")

	add_test(NAME t1_state_handoff_load_negative COMMAND t1_state_handoff_load_negative)

	# The pass condition is the report and not the exit status. halt_on_error
	# stops on the first race, so the run answers in seconds rather than
	# spending its whole window after it has already answered.
	#
	# abort_on_error=0 and exitcode=0 are load-bearing and not a loosening. On
	# Darwin the sanitizer raises SIGABRT on a report, and ctest scores an
	# aborted subprocess as an exception before it consults
	# PASS_REGULAR_EXPRESSION -- so the report this test exists to see would be
	# scored a failure. Exiting 0 hands the verdict to the regex instead, and a
	# run that reported no race prints no such line and fails.
	set_tests_properties(t1_state_handoff_load_negative PROPERTIES
		LABELS "IntegrationTest"
		TIMEOUT 900
		ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1 abort_on_error=0 exitcode=0"
		PASS_REGULAR_EXPRESSION "ThreadSanitizer: data race"
		SKIP_REGULAR_EXPRESSION "SKIPPED: ")

	if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
		set_property(TEST t1_state_handoff_load_negative APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
	endif()
endif()

# ----------------- the audio callback reaches the booted machine
#
# Check: ctest --test-dir build --no-tests=error -R ^t1_audio_reaches_machine$
#
# Tier T1 and gated. It boots the real Clavia firmware through rg2::Device::boot
# and then calls processAudio on that Device, so it resolves NMG2_ARTIFACTS
# through ArtifactResolver and reports the skip line when it is absent.
#
# It is the only audio test that installs no driver. t0_process_audio and
# t1_state_handoff_load both call installDriver() to substitute a recording
# driver, which is what lets them measure the call order -- and which replaces
# the one object a wiring question is about. This test asks whether the
# production driver reaches the Scheduler the boot produced, and the observable
# is the Scheduler's own virtual clock rather than the driver's own report.
#
# Its two controls are ungated and run even on a machine with no artifacts: they
# build their own Board and Scheduler and hand the predicate a SchedulerDriver
# over a Scheduler and one over none, so a skipped run keeps the evidence that
# the predicate separates a wired driver from an unwired one.
#
# SKIP_RETURN_CODE is what keeps a skipped run out of the Passed column. Without
# it a machine with no artifacts scores this test exactly as a machine that
# booted the firmware and drove the callback.
#
# TIMEOUT 600 guards the boot's 500,000-frame ceiling and not the measurement:
# the boot leaves early on Scheduler::chainAttached().

add_executable(t1_audio_reaches_machine
	t1_audio_reaches_machine.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2Device.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../rg2JucePlugin/rg2State.cpp)
target_link_libraries(t1_audio_reaches_machine PRIVATE rg2Lib)
set_property(TARGET t1_audio_reaches_machine PROPERTY FOLDER "RG2/test")

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/gatedFixture.h" g2_audioReachesSkipExitCodeLine REGEX "g_gatedSkipExitCode = [0-9]+")

if(NOT g2_audioReachesSkipExitCodeLine MATCHES "g_gatedSkipExitCode = ([0-9]+)")
	message(FATAL_ERROR "gatedFixture.h defines no g_gatedSkipExitCode, so ctest cannot be told which exit code is a skip")
endif()

set(g2_audioReachesSkipExitCode "${CMAKE_MATCH_1}")

add_test(NAME t1_audio_reaches_machine COMMAND t1_audio_reaches_machine)
set_tests_properties(t1_audio_reaches_machine PROPERTIES LABELS "IntegrationTest" TIMEOUT 600 SKIP_RETURN_CODE ${g2_audioReachesSkipExitCode})

if(IS_DIRECTORY "${NMG2_ARTIFACTS}")
	set_property(TEST t1_audio_reaches_machine APPEND PROPERTY ENVIRONMENT "NMG2_ARTIFACTS=${NMG2_ARTIFACTS}")
endif()
