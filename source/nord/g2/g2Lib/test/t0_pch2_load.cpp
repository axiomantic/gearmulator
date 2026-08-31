/* t0_pch2_load.cpp -- the `.pch2` load path without firmware.
 * Tier T0: no artifact, no firmware, and no Clavia byte -- every file it reads
 * is authored by this project.
 *
 * The property this file exists to hold: the plugin parses the file, then
 * drives the same protocol messages that an editor would drive, and has no
 * second, private load path. It gives the load path a `.pch2` container and
 * then reads what the transport hub drained -- not what the parser returned,
 * and not what a spy recorded inside it. A load path that parsed correctly and
 * originated nothing is green on a return-value test and red here.
 *
 * `fixtures/protocol/synth_editor_sequence.txt` is written by a walker
 * implemented in Python from the specification alone. It is a different
 * implementation of the same specification, in a different language, and it
 * read no C++. A fixture the C++ parser had produced would agree with the C++
 * parser by construction and would assert nothing.
 *
 * A green run proves the load path handles every case the specification names.
 * It proves nothing about real-world patch variety, because nobody wrote the
 * synthesized corpus from real patches. A construct a real Clavia patch uses
 * and the specification does not describe passes here and would fail against
 * the G2 Demo corpus, which is private.
 *
 * The comparison is one string against one string, and that is deliberate. A
 * per-row loop that compared only the rows it found would be green on a load
 * path that dropped a file, dropped an object, or invented one. The observed
 * transcript is built in the fixture's own format and the whole of it is
 * compared, so a missing row, an extra row and a wrong row are all one failure
 * mode with one cause.
 *
 * The escape, named rather than denied. A payload row carries a CRC-16 and not
 * the payload, so two different payloads of the same length that collide in
 * CRC-16 would pass. The load path never constructs a payload -- it forwards
 * the file's bytes -- so the mutation that would exploit the gap is a load path
 * that substituted one whole payload for a colliding one.
 */
#include "../../g2JucePlugin/g2PatchLoad.h"

#include "../crc16.h"
#include "../internalClient.h"
#include "../transportHub.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef G2_PCH2_SYNTH_CORPUS_DIR
#error "G2_PCH2_SYNTH_CORPUS_DIR is not defined; tests_proto.cmake defines it"
#endif

#ifndef G2_PCH2_EXPECTED_SEQUENCE
#error "G2_PCH2_EXPECTED_SEQUENCE is not defined; tests_proto.cmake defines it"
#endif

namespace
{
	int failures = 0;

	void check(const bool condition, const char* const what)
	{
		if(!condition)
		{
			printf("FAIL %s\n", what);
			++failures;
		}
	}

	/* The hub's derived ceiling: the wire framing is [1-byte type][2-byte
	 * length][payload], so one frame cannot exceed 1 + 2 + 65,535 bytes. The
	 * corpus drives a 65,469-byte payload, so a smaller ceiling here would turn
	 * a correct load into SendRefused and the check would report a parser fault
	 * that is really a test fault. */
	constexpr std::size_t kMaxFrameBytes = 1u + 2u + 0xFFFFu;

	/* Above the largest object count in the corpus, so the depth never binds.
	 * `object_types.pch2` carries the most, one for each named type. */
	constexpr std::size_t kQueueDepth = 32;

	std::vector<uint8_t> readFile(const std::string& _path)
	{
		std::ifstream in(_path.c_str(), std::ios::binary);
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
			std::istreambuf_iterator<char>());
	}

	std::string hex(const uint8_t* const _data, const std::size_t _size)
	{
		if(_size == 0)
			return "-";
		std::string out;
		char buf[3];
		for(std::size_t i = 0; i < _size; ++i)
		{
			snprintf(buf, sizeof(buf), "%02x", _data[i]);
			out += buf;
		}
		return out;
	}

	std::vector<std::string> split(const std::string& _text)
	{
		std::vector<std::string> out;
		std::istringstream in(_text);
		std::string row;
		while(std::getline(in, row))
			out.push_back(row);
		return out;
	}

	/* One file, loaded, with the hub drained afterwards. Returns the rows in
	 * the fixture's format. */
	std::vector<std::string> transcribe(const std::string& _name, const std::vector<uint8_t>& _bytes)
	{
		g2::TransportHub    hub(kMaxFrameBytes, kQueueDepth);
		g2::InternalClient  client(hub, kMaxFrameBytes, 4);

		const g2::Pch2LoadResult result = g2::pch2Load(_bytes.data(), _bytes.size(), client);

		std::vector<g2::StampedFrame> drained(kQueueDepth);
		const std::size_t got = hub.drainToDevice(drained.data(), drained.size());

		std::vector<std::string> rows;
		char buf[256];

		if(result != g2::Pch2LoadResult::Loaded)
		{
			snprintf(buf, sizeof(buf), "%s\tREFUSED\t%s", _name.c_str(),
				g2::pch2LoadResultName(result));
			rows.emplace_back(buf);
		}

		std::size_t consumed = 0;

		for(std::size_t i = 0; i < got; ++i)
		{
			const g2::ProtocolFrame& frame = drained[i].frame;

			/* A frame shorter than its own header cannot be transcribed at
			 * all, and reporting it as such is more use than reading past it. */
			if(frame.data == nullptr || frame.size < 3)
			{
				snprintf(buf, sizeof(buf), "%s\tFRAME\t%zu\tMALFORMED-FRAME\t%zu\t-",
					_name.c_str(), i, frame.size);
				rows.emplace_back(buf);
				continue;
			}

			const unsigned    type   = frame.data[0];
			const std::size_t length = (static_cast<std::size_t>(frame.data[1]) << 8) | frame.data[2];

			/* THE DECLARED LENGTH IS CHECKED AGAINST THE FRAME THE HUB
			 * ACTUALLY CARRIED. A load path that framed the header correctly
			 * and copied the wrong number of payload bytes agrees with the
			 * fixture on type and length and disagrees here. */
			check(frame.size == 3u + length,
				"the frame the hub drained is exactly its 3-byte header plus its declared payload");

			const uint16_t payloadCrc = g2::crc16(frame.data + 3, length);

			snprintf(buf, sizeof(buf), "%s\tFRAME\t%zu\t0x%02X\t%zu\t0x%04X",
				_name.c_str(), i, type, length, payloadCrc);
			rows.emplace_back(buf);

			consumed += frame.size;
		}

		if(result == g2::Pch2LoadResult::Loaded)
		{
			/* The trailer is what the container holds between the last object
			 * and the stored CRC. Recovered here from the file rather than
			 * from the load path, so the two disagree when the load path
			 * mis-walks the tail. */
			std::size_t nul = 0;
			while(nul < _bytes.size() && _bytes[nul] != 0)
				++nul;

			const std::size_t bodyStart = nul + 1u + 2u;
			const std::size_t bodyEnd   = _bytes.size() >= 2u ? _bytes.size() - 2u : 0u;
			const std::size_t tail      = (bodyEnd > bodyStart + consumed) ? bodyEnd - bodyStart - consumed : 0u;

			snprintf(buf, sizeof(buf), "%s\tTRAILER\t%s", _name.c_str(),
				hex(_bytes.data() + bodyStart + consumed, tail).c_str());
			rows.emplace_back(buf);
		}

		return rows;
	}
}

int main()
{
	const std::string corpus  = G2_PCH2_SYNTH_CORPUS_DIR;
	const std::string fixture = G2_PCH2_EXPECTED_SEQUENCE;

	/* THE FILE LIST COMES FROM THE CORPUS'S OWN MANIFEST, NOT FROM A DIRECTORY
	 * SCAN. `MANIFEST.tsv` is written by the generator that writes the corpus,
	 * so a file the generator stopped emitting leaves the list at once, and a
	 * stray `.pch2` somebody dropped into the directory cannot silently join
	 * the run. std::filesystem is unavailable at this deployment target, so a
	 * scan was not on offer either; the manifest is the better of the two and
	 * not a substitute for one.
	 *
	 * THE CHECK HAS NO GATE. A corpus that is not there is a FAILURE of the
	 * check and never a skip: a skipped T0 returns 0 and counts as a pass. */
	std::vector<std::string> files;
	{
		const std::string manifestPath = corpus + "/MANIFEST.tsv";
		std::ifstream manifest(manifestPath.c_str());
		if(!manifest)
		{
			printf("FAIL the synthesized corpus manifest is not at %s\n", manifestPath.c_str());
			return 1;
		}

		std::string row;
		while(std::getline(manifest, row))
		{
			if(row.empty() || row[0] == '#')
				continue;
			const std::string::size_type tab = row.find('\t');
			files.push_back(tab == std::string::npos ? row : row.substr(0, tab));
		}
	}

	std::sort(files.begin(), files.end());

	/* A manifest that parsed to nothing would produce an empty transcript, and
	 * an empty transcript is not obviously wrong until something says so. */
	check(!files.empty(), "the synthesized corpus manifest lists at least one file");

	std::string observed;
	for(const std::string& name : files)
	{
		const std::vector<uint8_t> bytes = readFile(corpus + "/" + name);
		check(!bytes.empty(), (std::string("the corpus file ") + name + " was read and is not empty").c_str());

		for(const std::string& row : transcribe(name, bytes))
		{
			observed += row;
			observed += '\n';
		}
	}

	std::ifstream in(fixture.c_str());
	if(!in)
	{
		printf("FAIL the expected sequence is not at %s\n", fixture.c_str());
		return 1;
	}

	std::string expected;
	std::string row;
	while(std::getline(in, row))
	{
		if(!row.empty() && row[0] == '#')
			continue;
		expected += row;
		expected += '\n';
	}

	check(!expected.empty(), "the expected sequence holds at least one row");

	if(observed != expected)
	{
		++failures;
		printf("FAIL the originated message sequence differs from the expected sequence\n");

		const std::vector<std::string> o = split(observed);
		const std::vector<std::string> e = split(expected);

		printf("  observed %zu row(s), expected %zu row(s)\n", o.size(), e.size());

		for(std::size_t n = 0; n < (o.size() > e.size() ? o.size() : e.size()); ++n)
		{
			const bool same = n < o.size() && n < e.size() && o[n] == e[n];
			if(!same)
				printf("  row %zu\n    observed: %s\n    expected: %s\n", n,
					n < o.size() ? o[n].c_str() : "<absent>",
					n < e.size() ? e[n].c_str() : "<absent>");
		}
	}

	if(failures != 0)
	{
		printf("t0_pch2_load: %d failure(s)\n", failures);
		return 1;
	}

	printf("t0_pch2_load: all cases passed\n");
	return 0;
}
