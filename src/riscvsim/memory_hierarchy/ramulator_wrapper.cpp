/**
 * Ramulator CPP wrapper
 *
 * MARSS-RISCV : Micro-Architectural System Simulator for RISC-V
 *
 * Copyright (c) 2020 Gaurav Kothari {gkothar1@binghamton.edu}
 * State University of New York at Binghamton
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "ramulator_wrapper.h"

ramulator_wrapper::ramulator_wrapper(const char *config_file,
                                     int cache_line_size)
    : read_cb_func(std::bind(&ramulator_wrapper::read_complete, this,
                             std::placeholders::_1)),
      write_cb_func(std::bind(&ramulator_wrapper::write_complete, this,
                              std::placeholders::_1))

{
    Config configs(config_file);
    configs.set_core_num(1);
    gem5_wrapper = new Gem5Wrapper(configs, cache_line_size);
    mem_access_active = false;
}

ramulator_wrapper::~ramulator_wrapper()
{
    delete gem5_wrapper;
}

void
ramulator_wrapper::finish()
{
    gem5_wrapper->finish();
}

void
ramulator_wrapper::read_complete(ramulator::Request &req)
{
    mem_access_active = false;
}

void
ramulator_wrapper::write_complete(Request &req)
{
    mem_access_active = false;
}

bool
ramulator_wrapper::add_transaction(target_ulong addr, bool isWrite)
{
    bool request_sent = false;

    if (isWrite)
    {
        ramulator::Request req((long)addr, ramulator::Request::Type::WRITE,
                               write_cb_func);
        request_sent = gem5_wrapper->send(req);
    }
    else
    {
        ramulator::Request req(addr, ramulator::Request::Type::READ,
                               read_cb_func);
        request_sent = gem5_wrapper->send(req);
    }

    mem_access_active = true;
    return request_sent;
}

int
ramulator_wrapper::get_max_clock_cycles()
{
    int clock_cycles_elasped = 0;

    while (mem_access_active)
    {
        gem5_wrapper->tick();
        clock_cycles_elasped++;
    }

    return clock_cycles_elasped;
}

void
ramulator_wrapper::print_stats(const char *stats_dir, const char *timestamp)
{
    gem5_wrapper->print_stats(stats_dir, timestamp);
}
