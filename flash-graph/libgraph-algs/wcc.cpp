/**
 * Copyright 2014 Open Connectome Project (http://openconnecto.me)
 * Written by Da Zheng (zhengda1936@gmail.com)
 *
 * This file is part of FlashGraph.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifdef PROFILER
#include <google/profiler.h>
#endif

#include <vector>
#include <unordered_map>

#include "thread.h"
#include "io_interface.h"
#include "container.h"
#include "concurrency.h"

#include "vertex_index.h"
#include "graph_engine.h"
#include "graph_config.h"
#include "FG_vector.h"
#include "FGlib.h"
#include "ts_graph.h"

namespace {

class component_message: public vertex_message
{
	int id;
public:
	component_message(vertex_id_t id): vertex_message(
			sizeof(component_message), true) {
		this->id = id;
	}

	vertex_id_t get_id() const {
		return id;
	}
};

class wcc_vertex: public compute_vertex
{
protected:
	bool empty;
private:
	bool updated;
	vertex_id_t component_id;
public:
	wcc_vertex(vertex_id_t id): compute_vertex(id) {
		component_id = id;
		updated = true;
		empty = true;
	}

	bool is_empty(graph_engine &graph) const {
		return empty;
	}

	bool belong2component() const {
		return component_id != UINT_MAX;
	}

	vertex_id_t get_component_id() const {
		return component_id;
	}

	void run(vertex_program &prog) {
		vertex_id_t id = get_id();
		if (updated) {
			request_vertices(&id, 1);
			updated = false;
		}
	}

	void run(vertex_program &prog, const page_vertex &vertex);

	void run_on_message(vertex_program &, const vertex_message &msg1) {
		component_message &msg = (component_message &) msg1;
		if (msg.get_id() < component_id) {
			updated = true;
			component_id = msg.get_id();
		}
	}

	vertex_id_t get_result() const {
		if (!empty)
			return get_component_id();
		else
			return INVALID_VERTEX_ID;
	}
};

void wcc_vertex::run(vertex_program &prog, const page_vertex &vertex)
{
	component_message msg(component_id);
	if (vertex.is_in_part()) {
		empty &= (vertex.get_num_edges(IN_EDGE) == 0);
		edge_seq_iterator it = vertex.get_neigh_seq_it(IN_EDGE, 0,
				vertex.get_num_edges(IN_EDGE));
		// We need to add the neighbors of the vertex to the queue of
		// the next level.
		prog.multicast_msg(it, msg);
	}
	else {
		empty &= (vertex.get_num_edges(OUT_EDGE) == 0);
		edge_seq_iterator it = vertex.get_neigh_seq_it(OUT_EDGE, 0,
				vertex.get_num_edges(OUT_EDGE));
		// We need to add the neighbors of the vertex to the queue of
		// the next level.
		prog.multicast_msg(it, msg);
	}
}

class ts_wcc_vertex: public wcc_vertex
{
public:
	ts_wcc_vertex(vertex_id_t id): wcc_vertex(id) {
	}

	void run(vertex_program &prog) {
		wcc_vertex::run(prog);
	}

	void run(vertex_program &prog, const page_vertex &vertex);
};

class ts_wcc_vertex_program: public vertex_program_impl<ts_wcc_vertex>
{
	time_t start_time;
	time_t time_interval;
public:
	ts_wcc_vertex_program(time_t start_time, time_t time_interval) {
		this->start_time = start_time;
		this->time_interval = time_interval;
	}

	time_t get_start_time() const {
		return start_time;
	}

	time_t get_time_interval() const {
		return time_interval;
	}
};

class ts_wcc_vertex_program_creater: public vertex_program_creater
{
	time_t start_time;
	time_t time_interval;
public:
	ts_wcc_vertex_program_creater(time_t start_time, time_t time_interval) {
		this->start_time = start_time;
		this->time_interval = time_interval;
	}

	vertex_program::ptr create() const {
		return vertex_program::ptr(new ts_wcc_vertex_program(
					start_time, time_interval));
	}
};

void ts_wcc_vertex::run(vertex_program &prog, const page_vertex &vertex)
{
	assert(prog.get_graph().is_directed());
	ts_wcc_vertex_program &wcc_vprog = (ts_wcc_vertex_program &) prog;
	const page_directed_vertex &dvertex = (const page_directed_vertex &) vertex;
	edge_seq_iterator in_it = get_ts_iterator(dvertex, edge_type::IN_EDGE,
			wcc_vprog.get_start_time(), wcc_vprog.get_time_interval());
	edge_seq_iterator out_it = get_ts_iterator(dvertex, edge_type::OUT_EDGE,
			wcc_vprog.get_start_time(), wcc_vprog.get_time_interval());
	empty = !in_it.has_next() && !out_it.has_next();
	component_message msg(get_component_id());
	prog.multicast_msg(in_it, msg);
	prog.multicast_msg(out_it, msg);
}

}

#include "save_result.h"
FG_vector<vertex_id_t>::ptr compute_wcc(FG_graph::ptr fg)
{
	graph_index::ptr index = NUMA_graph_index<wcc_vertex>::create(
			fg->get_index_file());
	graph_engine::ptr graph = graph_engine::create(fg->get_graph_file(),
			index, fg->get_configs());
	printf("weakly connected components starts\n");
#ifdef PROFILER
	if (!graph_conf.get_prof_file().empty())
		ProfilerStart(graph_conf.get_prof_file().c_str());
#endif

	struct timeval start, end;
	gettimeofday(&start, NULL);
	graph->start_all();
	graph->wait4complete();
	gettimeofday(&end, NULL);
	printf("WCC takes %f seconds\n", time_diff(start, end));

#ifdef PROFILER
	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
#endif

	FG_vector<vertex_id_t>::ptr vec = FG_vector<vertex_id_t>::create(graph);
	graph->query_on_all(vertex_query::ptr(
				new save_query<vertex_id_t, wcc_vertex>(vec)));
	return vec;
}

FG_vector<vertex_id_t>::ptr compute_ts_wcc(FG_graph::ptr fg,
		time_t start_time, time_t time_interval)
{
	graph_index::ptr index = NUMA_graph_index<ts_wcc_vertex>::create(
			fg->get_index_file());
	graph_engine::ptr graph = graph_engine::create(fg->get_graph_file(),
			index, fg->get_configs());
	assert(graph->get_graph_header().has_edge_data());
	printf("TS weakly connected components starts\n");
#ifdef PROFILER
	if (!graph_conf.get_prof_file().empty())
		ProfilerStart(graph_conf.get_prof_file().c_str());
#endif

	struct timeval start, end;
	gettimeofday(&start, NULL);
	graph->start_all(vertex_initializer::ptr(), vertex_program_creater::ptr(
				new ts_wcc_vertex_program_creater(start_time, time_interval)));
	graph->wait4complete();
	gettimeofday(&end, NULL);
	printf("WCC takes %f seconds\n", time_diff(start, end));

#ifdef PROFILER
	if (!graph_conf.get_prof_file().empty())
		ProfilerStop();
#endif

	FG_vector<vertex_id_t>::ptr vec = FG_vector<vertex_id_t>::create(graph);
	graph->query_on_all(vertex_query::ptr(
				new save_query<vertex_id_t, wcc_vertex>(vec)));
	return vec;
}
