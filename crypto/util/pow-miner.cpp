/* 
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give permission 
    to link the code of portions of this program with the OpenSSL library. 
    You must obey the GNU General Public License in all respects for all 
    of the code used other than OpenSSL. If you modify file(s) with this 
    exception, you may extend this exception to your version of the file(s), 
    but you are not obligated to do so. If you do not wish to do so, delete this 
    exception statement from your version. If you delete this exception statement 
    from all source files in the program, then also delete it here.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "common/bigint.hpp"
#include "common/refint.h"
#include "block/block.h"
#include "td/utils/benchmark.h"
#include "td/utils/filesystem.h"
#include "vm/boc.h"
#include "openssl/digest.hpp"
#include <openssl/sha.h>
#include <iostream>
#include <thread>
#include <cstdlib>
#include <getopt.h>
#include "git.h"
#include "Miner.h"

#if defined MINERCUDA
#include "cuda/miner.h"
#include "cuda/sha256.h"
#include "cuda/cuda.hpp"
#include "cuda/cuda_helper.h"
#endif

#if defined MINEROPENCL
#include "opencl/miner.h"
#include "opencl/opencl.h"
#include "opencl/sha256.h"
#endif

#if defined MINERCUDA || defined MINEROPENCL
#define MAX_THREADS 1
#else
#define MAX_THREADS 256
#endif

const char* progname;

int usage() {
  std::cerr << "usage: " << progname
            << " [-v][-B][-w<threads>]"
#if defined MINERCUDA || defined MINEROPENCL
               "[-g<gpu-id>]"
               "[-G<gpu-threads>]"
#endif
               " [-t<timeout>] <my-address> <pow-seed> <pow-complexity> <iterations> "
               "[<miner-addr> <output-ext-msg-boc>] [-V]\n"
               "Outputs a valid <rdata> value for proof-of-work testgiver after computing at most <iterations> hashes "
               "or terminates with non-zero exit code\n";
  std::exit(2);
}

td::RefInt256 parse_bigint(std::string str, int bits) {
  int len = (int)str.size();
  auto num = td::make_refint();
  auto& x = num.write();
  if (len >= 3 && str[0] == '0' && str[1] == 'x') {
    if (x.parse_hex(str.data() + 2, len - 2) != len - 2) {
      return {};
    }
  } else if (!len || x.parse_dec(str.data(), len) != len) {
    return {};
  }
  return x.unsigned_fits_bits(bits) ? std::move(num) : td::RefInt256{};
}

td::RefInt256 parse_bigint_chk(std::string str, int bits) {
  auto x = parse_bigint(std::move(str), bits);
  if (x.is_null()) {
    std::cerr << "fatal: `" << str << "` is not an integer" << std::endl;
    usage();
  }
  return x;
}

void parse_addr(std::string str, block::StdAddress& addr) {
  if (!addr.parse_addr(str) || (addr.workchain != -1 && addr.workchain != 0)) {
    std::cerr << "fatal: `" << str.c_str() << "` is not a valid blockchain address" << std::endl;
    usage();
  }
}

bool make_boc = false;
std::string boc_filename;
block::StdAddress miner_address;

int verbosity = 0;
std::atomic<td::uint64> hashes_computed{0};
td::Timestamp start_at;
td::CancellationTokenSource token;
bool boc_created = false;

void print_stats() {
  auto passed = td::Timestamp::now().at() - start_at.at();
  if (passed < 1e-9) {
    passed = 1;
  }
  std::cerr << "[ hashes computed: " << hashes_computed << " ]" << std::endl;
  std::cerr << "[ speed: " << static_cast<double>(hashes_computed) / passed << " hps ]" << std::endl;
}

int found(td::Slice data) {
  for (unsigned i = 0; i < data.size(); i++) {
    printf("%02X", data.ubegin()[i]);
  }
  printf("\n");
  if (make_boc) {
    vm::CellBuilder cb;
    td::Ref<vm::Cell> ext_msg, body;
    CHECK(cb.store_bytes_bool(data)                              // body
          && cb.finalize_to(body)                                // -> body
          && cb.store_long_bool(0x44, 7)                         // ext_message_in$10 ...
          && cb.store_long_bool(miner_address.workchain, 8)      // workchain
          && cb.store_bytes_bool(miner_address.addr.as_slice())  // miner addr
          && cb.store_long_bool(1, 6)                            // amount:Grams ...
          && cb.store_ref_bool(std::move(body))                  // body:^Cell
          && cb.finalize_to(ext_msg));
    auto boc = vm::std_boc_serialize(std::move(ext_msg), 2).move_as_ok();
    std::cerr << "Saving " << boc.size() << " bytes of serialized external message into file `" << boc_filename << "`"
              << std::endl;
    td::write_file(boc_filename, boc).ensure();
  }
  token.cancel();
  boc_created = true;
  return 0;
}

void miner(const ton::Miner::Options& options, const int thread_id) {
#if defined MINERCUDA
  // init cuda device for thread
  cudaSetDevice(options.gpu_id);
  cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
  cudaDeviceSetCacheConfig(cudaFuncCachePreferL1);

  auto res = ton::MinerCuda::run(options, thread_id);
#elif defined MINEROPENCL
  auto res = ton::MinerOpenCL::run(options, thread_id);
#else
  auto res = ton::Miner::run(options, thread_id);
#endif
  if (res) {
    found(res.value());
    // exit immediately, won't wait for all threads to terminate
    // note: this action causes the cuda error in func 'bitcredit_cpu_hash' at line 467 : driver shutting down.
    //std::exit(0);
  }
}

class MinerBench : public td::Benchmark {
 public:
  MinerBench(int gpu_id) {
    gpu_id_ = gpu_id;
  }

  std::string get_description() const override {
    return "Miner";
  }

  void run(int n) override {
    ton::Miner::Options options;
    options.my_address.parse_addr("EQDU86V5wyPrLd4nQ0RHPcCLPZq_y1O5wFWyTsMw63vjXTOv");
    std::fill(options.seed.begin(), options.seed.end(), 0xa7);
    std::fill(options.complexity.begin(), options.complexity.end(), 0);
    options.gpu_id = gpu_id_;
#if defined MINERCUDA
    options.max_iterations = 0xfffffff;
    CHECK(!ton::MinerCuda::run(options, 0));  // same thread_id for all runs
#elif defined MINEROPENCL
    options.max_iterations = 0xfffffff;
    CHECK(!ton::MinerOpenCL::run(options, 0));
#else
    options.max_iterations = n;
    CHECK(!ton::Miner::run(options, 0));
#endif
  }

 private:
  int gpu_id_;
};

int main(int argc, char* const argv[]) {
  ton::Miner::Options options;

  progname = argv[0];
  int i, threads = 1, gpu_threads, gpu_id = -1;
  bool bounce = false, benchmark = false;
  while ((i = getopt(argc, argv, "bnvw:g:G:t:Bh:V")) != -1) {
    switch (i) {
      case 'v':
        ++verbosity;
        break;
      case 'w':
#if !defined MINERCUDA && !defined MINEROPENCL
        threads = atoi(optarg);
        CHECK(threads > 0 && threads <= 256); // MAX_THREADS
#endif
        options.threads = threads;
        break;
#if defined MINERCUDA || defined MINEROPENCL
      case 'g':
        gpu_id = atoi(optarg);
        CHECK(gpu_id >= 0 && gpu_id <= 16);
        break;
      case 'G':
        gpu_threads = atoi(optarg);
        CHECK(gpu_threads >= 0 && gpu_threads <= 1792);  // MAX_GPU_THREADS
        options.gpu_threads = gpu_threads;
        break;
#endif
      case 't': {
        int timeout = atoi(optarg);
        CHECK(timeout > 0);
        options.expire_at = td::Timestamp::in(timeout);
        break;
      }
      case 'B':
        benchmark = true;
        break;
      case 'b':
        bounce = true;
        break;
      case 'n':
        bounce = false;
        break;
      case 'V':
        std::cout << "pow-miner build information: [ Commit: " << GitMetadata::CommitSHA1()
                  << ", Date: " << GitMetadata::CommitDate() << "]\n";
        std::exit(0);
        break;
      case 'h':
        return usage();
      default:
        std::cerr << "unknown option" << std::endl;
        return usage();
    }
  }
#if defined MINERCUDA
  for (i = 0; i < MAX_GPUS; i++) {
    device_map[i] = i;
    device_name[i] = NULL;
  }
  cuda_devicenames();
  cuda_print_devices();
  if (gpu_id < 0) {
    std::cerr << "unknown GPU ID" << std::endl;
    return usage();
  }
  std::atexit(cuda_shutdown);
#endif

#if defined MINEROPENCL
  if (gpu_id < 0) {
    auto opencl = opencl::OpenCL();
    opencl.print_devices();
    std::cerr << "unknown GPU ID" << std::endl;
    return usage();
  }
#endif

  options.gpu_id = gpu_id;
  options.token_ = token.get_cancellation_token();

  if (benchmark && argc == optind) {
    td::bench(MinerBench(gpu_id));
    return 0;
  }

  if (argc != optind + 4 && argc != optind + 6) {
    return usage();
  }

  parse_addr(argv[optind], options.my_address);
  options.my_address.bounceable = bounce;
  CHECK(parse_bigint_chk(argv[optind + 1], 128)->export_bytes(options.seed.data(), 16, false));

  auto cmplx = parse_bigint_chk(argv[optind + 2], 256);
  CHECK(cmplx->export_bytes(options.complexity.data(), 32, false));
  CHECK(!cmplx->unsigned_fits_bits(256 - 62));
  td::BigInt256 bigpower, hrate;
  bigpower.set_pow2(256).mod_div(*cmplx, hrate);
  long long hash_rate = hrate.to_long();
  options.max_iterations = parse_bigint_chk(argv[optind + 3], 64)->to_long();
  if (argc == optind + 6) {
    make_boc = true;
    parse_addr(argv[optind + 4], miner_address);
    boc_filename = argv[optind + 5];
  }

  if (verbosity >= 2) {
    std::cerr << "[ expected required hashes for success: " << hash_rate << " ]" << std::endl;
  }
  if (benchmark) {
    td::bench(MinerBench(gpu_id));
  }

  start_at = td::Timestamp::now();

  options.hashes_computed = &hashes_computed;
  // may invoke several miner threads
  if (threads <= 0) {
    miner(options, 0);
  } else {
    std::vector<std::thread> T;
    for (int i = 0; i < threads; i++) {
      T.emplace_back(miner, options, i);
    }
    for (auto& thr : T) {
      thr.join();
    }
  }
  if (verbosity > 0) {
    print_stats();
  }
  if (!boc_created) {
    std::exit(1);
  }
}
