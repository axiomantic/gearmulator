// Task BRD-19. Tier T0: this test needs no firmware artifact of any kind.
//
// Plan section 13.6, BRD-19. Design section 10.6 and 24.
// Logbook: AGENTS.md section 3.1.
//
// NO ASSERTION IN THIS FILE IS A LANGUAGE assert(). The default build is
// Release and it defines NDEBUG.
//
// THE PROTOCOL, AS THE FIRMWARE DRIVES IT. Design section 10.6: the OS pushes
// a COUNT word, an ADDRESS word of 0x000000, and N DATA words through the
// HDI08, with NO CVR host command. The model is the factory mask-ROM bootstrap
// loader that turns that stream into the words that land in P memory.
//
// THE TEST DRIVES THE PROTOCOL AGAINST A SYNTHETIC IMAGE. Every word below is
// authored here; nothing is read from a firmware artifact. The P memory is a
// plain array, because the bootstrap writes through a pointer and the DSP set
// does not exist on the board track yet.
//
// WHAT DISTINGUISHES A MODELLED BOOTSTRAP FROM A PRE-LOAD. Before any word is
// fed, the model has written nothing: P:$0 holds the sentinel, not the data.
// The words land because the model moves them, and that is the assertion that
// makes pre-loading undetectable only if the test never checks the before
// state. This test checks it, in case group 1.

#include "hdi08Bootstrap.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace
{
	int g_failures = 0;
	int g_cases = 0;

	void check(const bool _condition, const std::string& _what)
	{
		++g_cases;
		if(_condition)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << std::endl;
		++g_failures;
	}

	template<typename T>
	void checkEqual(const T& _actual, const T& _expected, const std::string& _what)
	{
		++g_cases;
		if(_actual == _expected)
		{
			std::cout << "ok   " << _what << std::endl;
			return;
		}
		std::cout << "FAIL " << _what << ": expected <" << _expected
			<< ">, got <" << _actual << ">" << std::endl;
		++g_failures;
	}

	// A small P memory, every word pre-set to a sentinel that no fed word will
	// collide with.
	constexpr uint32_t g_sentinel = 0xDEADBEu;
	constexpr uint32_t g_capacity = 64u;
}

int main()
{
	// -----------------------------------------------------------------------
	// Case group 0. A FRESH MACHINE HAS WRITTEN NOTHING.
	//
	// The state machine starts waiting for the count, reports no data
	// received, and is not complete. This is the "before the push" that the
	// distinction from a pre-load depends on.
	{
		uint32_t p[g_capacity];
		for(uint32_t& w : p) w = g_sentinel;

		g2::Hdi08Bootstrap boot(p, g_capacity);

		check(boot.status() == g2::Hdi08Bootstrap::Status::WaitingForCount,
			"a fresh bootstrap waits for the count word");
		check(!boot.isComplete(),
			"a fresh bootstrap is not complete");
		checkEqual(boot.receivedDataWords(), uint32_t(0),
			"a fresh bootstrap has received no data words");
		checkEqual(p[0], g_sentinel,
			"before the push, P:$0 holds none of the image, which separates a modelled bootstrap from a pre-load");
	}

	// -----------------------------------------------------------------------
	// Case group 1. THE THREE-HEADER, THEN BODY, THEN COMPLETE PATH.
	//
	// The full protocol: count N, address 0x000000, then N synthetic data
	// words with no CVR host command. Every data word lands at P:address+i in
	// order.
	{
		uint32_t p[g_capacity];
		for(uint32_t& w : p) w = g_sentinel;

		constexpr uint32_t count = 5u;
		constexpr uint32_t address = 0u;
		const uint32_t data[count] = {0x000001u, 0x000002u, 0x000003u, 0x000004u, 0x000005u};

		g2::Hdi08Bootstrap boot(p, g_capacity);

		checkEqual(static_cast<int>(boot.feed(count)), static_cast<int>(g2::Hdi08Bootstrap::Status::WaitingForAddress),
			"the count word moves the machine to WaitingForAddress");
		checkEqual(static_cast<int>(boot.feed(address)), static_cast<int>(g2::Hdi08Bootstrap::Status::Receiving),
			"the address word moves the machine to Receiving");

		// The sentinel is untouched through the two headers: no header is data.
		checkEqual(p[0], g_sentinel,
			"after the two header words, P:$0 still holds the sentinel");

		g2::Hdi08Bootstrap::Status after = g2::Hdi08Bootstrap::Status::WaitingForCount;
		for(uint32_t i = 0; i < count; ++i)
		{
			after = boot.feed(data[i]);
			checkEqual(p[i], data[i],
				"data word " + std::to_string(i) + " lands at P:$" + std::to_string(i) + " in order");
			checkEqual(boot.receivedDataWords(), i + 1u,
				"after data word " + std::to_string(i) + ", the received count is " + std::to_string(i + 1));
		}

		check(after == g2::Hdi08Bootstrap::Status::Complete,
			"after all N data words the bootstrap is complete");
		check(boot.isComplete(),
			"a fully fed load reports complete");
		checkEqual(boot.count(), count, "the count read back matches what was fed");
		checkEqual(boot.address(), address, "the address read back matches what was fed");
		checkEqual(boot.receivedDataWords(), count,
			"the received-data count equals the promised count N");

		// Every land recorded: the five stored slots hold the data and nothing
		// beyond them was touched.
		checkEqual(p[count], g_sentinel,
			"no word is stored beyond the N the count promised");
	}

	// -----------------------------------------------------------------------
	// Case group 2. AN ADDRESS OTHER THAN ZERO LANDS WHERE THE ADDRESS SAYS.
	//
	// The firmware pushes 0x000000, but the model must not special-case it:
	// whatever address the host chooses is where the body lands.
	{
		uint32_t p[g_capacity];
		for(uint32_t& w : p) w = g_sentinel;

		constexpr uint32_t count = 3u;
		constexpr uint32_t address = 10u;
		const uint32_t data[count] = {0x101u, 0x202u, 0x303u};

		g2::Hdi08Bootstrap boot(p, g_capacity);
		boot.feed(count);
		boot.feed(address);
		for(uint32_t i = 0; i < count; ++i)
			boot.feed(data[i]);

		check(boot.isComplete(), "a non-zero-address load is complete");
		checkEqual(p[10], uint32_t(0x101u), "data lands at the stated address");
		checkEqual(p[12], uint32_t(0x303u), "data lands in order after the stated address");
		checkEqual(p[9], g_sentinel, "nothing is written before the stated address");
		checkEqual(p[13], g_sentinel, "nothing is written after the stated body");
	}

	// -----------------------------------------------------------------------
	// Case group 3. THE NEGATIVE CASE: A STREAM THAT STOPS SHORT.
	//
	// A count word of N followed by only N - 1 data words. The model must
	// report an incomplete load, not dispatch: isComplete() is false, and the
	// machine is still Receiving because the body is not finished. This is
	// the plan's "reports an incomplete load rather than dispatching".
	{
		uint32_t p[g_capacity];
		for(uint32_t& w : p) w = g_sentinel;

		constexpr uint32_t count = 5u;
		constexpr uint32_t address = 0u;
		const uint32_t data[count - 1u] = {0x1u, 0x2u, 0x3u, 0x4u};

		g2::Hdi08Bootstrap boot(p, g_capacity);
		boot.feed(count);
		boot.feed(address);
		for(uint32_t i = 0; i < count - 1u; ++i)
			boot.feed(data[i]);

		check(!boot.isComplete(),
			"a count of N with only N-1 data words is NOT complete and does not dispatch");
		check(boot.status() == g2::Hdi08Bootstrap::Status::Receiving,
			"the short load is still Receiving, waiting for the missing word");
		checkEqual(boot.receivedDataWords(), count - 1u,
			"the short load has received exactly the N-1 words it was fed");
		checkEqual(p[count - 1u], g_sentinel,
			"the missing word's slot was never written");
	}

	// -----------------------------------------------------------------------
	// Case group 4. A COUNT OF ZERO COMPLETES AT THE FIRST WORD.
	//
	// The count is the only bound the protocol carries. A count of zero
	// promises an empty body, so the machine completes as soon as the address
	// arrives -- no data word can make it "more" complete, and there is no
	// end-of-transfer marker to wait for.
	{
		uint32_t p[g_capacity];
		for(uint32_t& w : p) w = g_sentinel;

		g2::Hdi08Bootstrap boot(p, g_capacity);
		boot.feed(0u);
		g2::Hdi08Bootstrap::Status after = boot.feed(0u);

		check(after == g2::Hdi08Bootstrap::Status::Complete,
			"a count of zero is complete immediately after the address word");
		check(boot.isComplete(), "a count of zero reports complete");
		checkEqual(p[0], g_sentinel, "a count of zero stores nothing");
	}

	// -----------------------------------------------------------------------
	// Case group 5. THE WORDS ARE 24-BIT AND THE HIGH BYTE IS DISCARDED.
	//
	// A received word is only meaningful in its low 24 bits. The count and the
	// stored data both honor the mask, so a stray high byte cannot pollute
	// either the bound or the image.
	{
		uint32_t p[g_capacity];
		for(uint32_t& w : p) w = g_sentinel;

		g2::Hdi08Bootstrap boot(p, g_capacity);
		boot.feed(0x01000002u);   // high byte set; low 24 bits are the count 2
		boot.feed(0u);
		boot.feed(0xaa000011u);   // high byte set; low 24 bits are the data 0x11
		boot.feed(0x22000022u);

		check(boot.isComplete(), "a load whose words carry high bytes still completes");
		checkEqual(boot.count(), uint32_t(2), "the high byte of the count word is masked away");
		checkEqual(p[0], uint32_t(0x11u), "the high byte of the first data word is masked away");
		checkEqual(p[1], uint32_t(0x22u), "the high byte of the second data word is masked away");
	}

	// -----------------------------------------------------------------------
	// Case group 6. A STORE PAST THE SUPPLIED BUFFER IS REFUSED, NOT WRITTEN.
	//
	// Objects beyond the caller's buffer are not written and the refusal is
	// counted, so a caller that sized its image too small sees the loss
	// instead of a silent write past the end.
	{
		uint32_t p[4];
		for(uint32_t& w : p) w = g_sentinel;

		// Count 4, address 0, capacity 4: exactly fits, no refusal.
		g2::Hdi08Bootstrap across(p, 4u);
		across.feed(4u);
		across.feed(0u);
		for(uint32_t i = 0; i < 4u; ++i)
			across.feed(0x100u + i);
		check(across.isComplete(), "a body that exactly fills the buffer is complete");
		checkEqual(across.refusedStores(), uint32_t(0), "an exact-fit body refuses nothing");

		// Count 5, address 0, capacity 4: the fifth word has no slot.
		g2::Hdi08Bootstrap overflow(p, 4u);
		overflow.feed(5u);
		overflow.feed(0u);
		for(uint32_t i = 0; i < 5u; ++i)
			overflow.feed(0x100u + i);
		check(!overflow.isComplete(), "a body longer than the buffer is not complete");
		checkEqual(overflow.refusedStores(), uint32_t(1),
			"the word with no slot is refused and counted, not written");
		checkEqual(p[3], uint32_t(0x103u), "every in-bounds word still lands");
		check(overflow.status() == g2::Hdi08Bootstrap::Status::Receiving,
			"the overflow load is still Receiving because its body is not all stored");
	}

	// -----------------------------------------------------------------------
	// Case group 7. WORDS AFTER A COMPLETE LOAD ARE DISCARDED.
	//
	// The bootstrap knows exactly N words and reads none after them. A word
	// fed past the complete body changes nothing and writes nothing.
	{
		uint32_t p[g_capacity];
		for(uint32_t& w : p) w = g_sentinel;

		g2::Hdi08Bootstrap boot(p, g_capacity);
		boot.feed(1u);
		boot.feed(0u);
		boot.feed(0xabu);
		g2::Hdi08Bootstrap::Status after = boot.feed(0xcdu);

		check(after == g2::Hdi08Bootstrap::Status::Complete,
			"a word after a complete load does not change the status");
		checkEqual(p[1], g_sentinel, "a word after the complete body is not stored");
		checkEqual(boot.receivedDataWords(), uint32_t(1), "a word after the complete body is not counted as data");
	}

	if(g_failures)
	{
		std::cout << "t0_bootstrap_rom: " << g_failures << " of " << g_cases
			<< " cases failed" << std::endl;
		return 1;
	}

	std::cout << "t0_bootstrap_rom: " << g_cases << " of " << g_cases
		<< " cases passed" << std::endl;
	return 0;
}
