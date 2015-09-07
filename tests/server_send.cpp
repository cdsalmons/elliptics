/*
 * 2015+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "test_base.hpp"
#include "../library/elliptics.h"
#include <algorithm>

#define BOOST_TEST_NO_MAIN
#include <boost/test/included/unit_test.hpp>

#include <boost/program_options.hpp>

using namespace ioremap::elliptics;
using namespace boost::unit_test;

static std::shared_ptr<tests::nodes_data> ssend_servers;

static std::vector<int> ssend_src_groups = {1};
static std::vector<int> ssend_dst_groups = {2, 3};
static size_t ssend_backends = 8;

static std::string print_groups(const std::vector<int> &groups) {
	std::ostringstream ss;
	for (size_t pos = 0; pos < groups.size(); ++pos) {
		ss << groups[pos];
		if (pos != groups.size() - 1)
			ss << ":";
	}

	return ss.str();
}

static tests::server_config ssend_server_config(int group)
{
	// Minimize number of threads
	tests::server_config server = tests::server_config::default_value();
	server.options
		("io_thread_num", 4)
		("nonblocking_io_thread_num", 4)
		("net_thread_num", 1)
		("caches_number", 1)
	;

	server.backends[0]("enable", true)("group", group);
	server.backends.resize(ssend_backends, server.backends.front());

	return server;
}

static void ssend_configure(const std::string &path)
{
	std::vector<tests::server_config> servers;
	for (const auto &g : ssend_src_groups) {
		tests::server_config server = ssend_server_config(g);
		servers.push_back(server);
	}
	for (const auto &g : ssend_dst_groups) {
		tests::server_config server = ssend_server_config(g);
		servers.push_back(server);
	}

	tests::start_nodes_config cfg(results_reporter::get_stream(), std::move(servers), path);
	cfg.fork = true;

	ssend_servers = tests::start_nodes(cfg);
}

static void ssend_test_insert_many_keys(session &s, int num, const std::string &id_prefix, const std::string &data_prefix)
{
	for (int i = 0; i < num; ++i) {
		std::string id = id_prefix + lexical_cast(i);
		std::string data = data_prefix + lexical_cast(i);

		ELLIPTICS_REQUIRE(res, s.write_data(id, data, 0));
	}
}

static void ssend_test_read_many_keys(session &s, int num, const std::string &id_prefix, const std::string &data_prefix)
{
	BH_LOG(s.get_logger(), DNET_LOG_NOTICE, "%s: session groups: %s, num: %d",
		__func__, print_groups(s.get_groups()), num);

	for (int i = 0; i < num; ++i) {
		std::string id = id_prefix + lexical_cast(i);
		std::string data = data_prefix + lexical_cast(i);

		ELLIPTICS_COMPARE_REQUIRE(res, s.read_data(id, 0, 0), data);
	}
}


static void ssend_test_copy(session &s, const std::vector<int> &dst_groups, int num)
{
	auto run_over_single_backend = [] (session &s, const key &id, const std::vector<int> &dst_groups) {
		std::vector<dnet_iterator_range> ranges;
		dnet_iterator_range whole;
		memset(whole.key_begin.id, 0, sizeof(dnet_raw_id));
		memset(whole.key_end.id, 0xff, sizeof(dnet_raw_id));
		ranges.push_back(whole);

		dnet_time time_begin, time_end;
		dnet_empty_time(&time_begin);
		dnet_current_time(&time_end);

		uint64_t iflags = DNET_IFLAGS_KEY_RANGE | DNET_IFLAGS_NO_META;

		auto iter = s.start_copy_iterator(id, ranges, DNET_ITYPE_SERVER_SEND, iflags, time_begin, time_end, dst_groups);

		int copied = 0;

		//char buffer[2*DNET_ID_SIZE + 1] = {0};

		logger &log = s.get_logger();

		for (auto it = iter.begin(), end = iter.end(); it != end; ++it) {
#if 0
			// we have to explicitly convert all members from dnet_iterator_response
			// since it is packed and there will be alignment issues and
			// following error:
			// error: cannot bind packed field ... to int&
			BH_LOG(log, DNET_LOG_DEBUG,
					"ssend_test: "
					"key: %s, backend: %d, user_flags: %llx, ts: %lld.%09lld, status: %d, size: %lld, "
					"iterated_keys: %lld/%lld",
				dnet_dump_id_len_raw(it->reply()->key.id, DNET_ID_SIZE, buffer),
				(int)it->command()->backend_id,
				(unsigned long long)it->reply()->user_flags,
				(unsigned long long)it->reply()->timestamp.tsec, (unsigned long long)it->reply()->timestamp.tnsec,
				(int)it->reply()->status, (unsigned long long)it->reply()->size,
				(unsigned long long)it->reply()->iterated_keys, (unsigned long long)it->reply()->total_keys);
#endif

			copied++;
		}

		BH_LOG(log, DNET_LOG_NOTICE, "ssend_test: %s: dst_groups: %s, copied: %d",
				id.to_string(),
				print_groups(dst_groups), copied);

		return copied;
	};

	std::set<uint32_t> backends;
	int copied = 0;

	std::vector<int> groups = s.get_groups();
	std::vector<dnet_route_entry> routes = s.get_routes();

	for (auto it = routes.begin(); it != routes.end(); ++it) {
		const dnet_route_entry &entry = *it;
		if (std::find(groups.begin(), groups.end(), entry.group_id) != groups.end()) {
			auto back = backends.find(entry.backend_id);
			if (back == backends.end()) {
				backends.insert(entry.backend_id);
				copied += run_over_single_backend(s, entry.id, dst_groups);
			}
		}
	}

	BOOST_REQUIRE_EQUAL(copied, num);
}

static bool ssend_register_tests(test_suite *suite, node &n)
{
	std::string id_prefix = "server send id";
	std::string data_prefix = "this is a test data";
	int num = 10000;

	session src(n);
	src.set_groups(ssend_src_groups);
	src.set_exceptions_policy(session::no_exceptions);

	ELLIPTICS_TEST_CASE(ssend_test_insert_many_keys, src, num, id_prefix, data_prefix);
	ELLIPTICS_TEST_CASE(ssend_test_copy, src, ssend_dst_groups, num);


	// check every dst group, it must contain all keys written into src groups
	for (const auto &g : ssend_dst_groups) {
		ELLIPTICS_TEST_CASE(ssend_test_read_many_keys, tests::create_session(n, {g}, 0, 0), num, id_prefix, data_prefix);
	}

	return true;
}

static void ssend_free_servers()
{
	ssend_servers.reset();
}

static boost::unit_test::test_suite *ssend_setup_tests(int argc, char *argv[])
{
	namespace bpo = boost::program_options;

	bpo::variables_map vm;
	bpo::options_description generic("Test options");

	std::string path;

	generic.add_options()
			("help", "This help message")
			("path", bpo::value(&path), "Path where to store everything")
			;

	bpo::store(bpo::parse_command_line(argc, argv, generic), vm);
	bpo::notify(vm);

	if (vm.count("help")) {
		std::cerr << generic;
		return NULL;
	}

	test_suite *suite = new test_suite("Local Test Suite");

	ssend_configure(path);
	ssend_register_tests(suite, *ssend_servers->node);

	return suite;
}

int main(int argc, char *argv[])
{
	atexit(ssend_free_servers);

	srand(time(0));
	return unit_test_main(ssend_setup_tests, argc, argv);
}
