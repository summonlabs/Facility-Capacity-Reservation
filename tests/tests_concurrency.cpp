// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// In-process concurrency: many threads mutating one store through its public
// API, and many threads observing it while it is mutated.
//
// The store serialises mutations with one non-recursive mutex and takes a
// snapshot inside the lock, so a caller never observes a half-applied state.
// What this suite proves is the in-process half of that claim; the cross-process
// half is proved with real operating-system processes in tests_multiprocess.cpp,
// and neither is claimed from the other.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

namespace fcr = dccp::facility_capacity_reservation;

namespace {

const std::vector<fcr_test::PoolSpec> kPools = {
    {fcr::ResourceKind::Rack, "hall-a", 200, 0, 0},
    {fcr::ResourceKind::Power, "feed-a", 400'000, 0, 0},
};

}  // namespace

FCR_TEST(concurrency, many_writers_close_the_accounting_exactly) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  constexpr int kThreads = 8;
  constexpr int kReservationsPerThread = 25;

  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&fixture, &accepted, &rejected, thread] {
      for (int index = 0; index < kReservationsPerThread; ++index) {
        const std::string id = "res-" + fcr::format_unsigned(static_cast<std::uint64_t>(thread)) + "-" +
                               fcr::format_unsigned(static_cast<std::uint64_t>(index));
        const std::string attempt_name = "attempt-" + fcr::format_unsigned(static_cast<std::uint64_t>(thread)) + "-" +
                                         fcr::format_unsigned(static_cast<std::uint64_t>(index));
        const fcr::Result<fcr::ReserveOutcome> outcome = fixture.store.reserve(fcr_test::reserve_request(
            fixture, id.c_str(), attempt_name.c_str(), {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100,
            900'000));
        if (outcome.has_value()) {
          accepted.fetch_add(1);
        } else {
          rejected.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  FCR_CHECK_EQ(accepted.load() + rejected.load(), kThreads * kReservationsPerThread);
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ((*pools)[0].committed, static_cast<std::uint64_t>(accepted.load()));
  FCR_CHECK_EQ((*pools)[0].free, 200ULL - static_cast<std::uint64_t>(accepted.load()));
  FCR_CHECK_EQ((*pools)[0].committed + (*pools)[0].protected_ + (*pools)[0].free, (*pools)[0].reservable);

  const fcr::Result<fcr::VerificationReport> report = fixture.store.verify();
  FCR_REQUIRE(report.has_value());
  FCR_CHECK(report->ok);
  const fcr::Result<fcr::LedgerStatus> status = fixture.store.status();
  FCR_REQUIRE(status.has_value());
  FCR_CHECK_EQ(status->reservation_count, static_cast<std::size_t>(accepted.load()));
}

FCR_TEST(concurrency, writers_and_releasers_settle_to_a_closed_account) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  constexpr int kThreads = 6;
  constexpr int kRounds = 20;

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&fixture, thread] {
      for (int round = 0; round < kRounds; ++round) {
        const std::string id = "res-" + fcr::format_unsigned(static_cast<std::uint64_t>(thread)) + "-" +
                               fcr::format_unsigned(static_cast<std::uint64_t>(round));
        const fcr::Result<fcr::ReserveOutcome> reserved = fixture.store.reserve(fcr_test::reserve_request(
            fixture, id.c_str(), ("attempt-" + id).c_str(), {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 2)},
            1'100, 900'000));
        if (!reserved.has_value()) {
          continue;
        }
        if (round % 2 == 0) {
          (void)fixture.store.release(fcr_test::release_request(
              fixture, id.c_str(), ("release-" + id).c_str(), reserved->reservation.record.generation));
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  // Exactly half of the successful rounds released their reservation.
  const std::uint64_t held = (*pools)[0].committed;
  FCR_CHECK_EQ(held % 2, 0ULL);
  FCR_CHECK_EQ(held + (*pools)[0].protected_ + (*pools)[0].free, (*pools)[0].reservable);

  const fcr::Result<std::vector<fcr::ReservationView>> views = fixture.store.list();
  FCR_REQUIRE(views.has_value());
  std::size_t active = 0;
  for (const fcr::ReservationView& view : *views) {
    if (view.active()) {
      ++active;
    }
  }
  FCR_CHECK_EQ(static_cast<std::uint64_t>(active) * 2ULL, held);
}

FCR_TEST(concurrency, observers_never_see_a_half_applied_state) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  std::atomic<bool> stop{false};
  std::atomic<int> observations{0};
  std::atomic<int> failures{0};

  std::vector<std::thread> writers;
  for (int thread = 0; thread < 4; ++thread) {
    writers.emplace_back([&fixture, thread] {
      for (int index = 0; index < 60; ++index) {
        const std::string id = "res-" + fcr::format_unsigned(static_cast<std::uint64_t>(thread)) + "-" +
                               fcr::format_unsigned(static_cast<std::uint64_t>(index));
        (void)fixture.store.reserve(fcr_test::reserve_request(
            fixture, id.c_str(), ("attempt-" + id).c_str(),
            {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1),
             fcr_test::claim(fcr::ResourceKind::Power, "feed-a", 1'000)},
            1'100, 900'000));
      }
    });
  }

  std::vector<std::thread> readers;
  for (int thread = 0; thread < 3; ++thread) {
    readers.emplace_back([&fixture, &stop, &observations, &failures] {
      // The reader loop is bounded so the test terminates on its own; it is not
      // a timeout, it is a fixed amount of work.
      for (int iteration = 0; iteration < 400 && !stop.load(); ++iteration) {
        const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
        if (!pools.has_value()) {
          failures.fetch_add(1);
          continue;
        }
        for (const fcr::PoolAccount& account : *pools) {
          if (account.committed + account.protected_ + account.free != account.reservable) {
            failures.fetch_add(1);
          }
        }
        const fcr::Result<fcr::VerificationReport> report = fixture.store.verify();
        if (!report.has_value() || !report->ok) {
          failures.fetch_add(1);
        }
        observations.fetch_add(1);
      }
    });
  }

  for (std::thread& writer : writers) {
    writer.join();
  }
  stop.store(true);
  for (std::thread& reader : readers) {
    reader.join();
  }

  FCR_CHECK(observations.load() > 0);
  FCR_CHECK_EQ(failures.load(), 0);
}

FCR_TEST(concurrency, identical_attempts_racing_across_threads_reserve_once) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  constexpr int kThreads = 8;
  const fcr::ReserveRequest request = fcr_test::reserve_request(
      fixture, "res-shared", "attempt-shared", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 5)}, 1'100, 900'000);

  std::atomic<int> applied{0};
  std::atomic<int> replayed{0};
  std::atomic<int> failed{0};
  std::vector<std::thread> workers;
  for (int thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&fixture, &request, &applied, &replayed, &failed] {
      const fcr::Result<fcr::ReserveOutcome> outcome = fixture.store.reserve(request);
      if (!outcome.has_value()) {
        failed.fetch_add(1);
      } else if (outcome->replayed) {
        replayed.fetch_add(1);
      } else {
        applied.fetch_add(1);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  FCR_CHECK_EQ(applied.load(), 1);
  FCR_CHECK_EQ(failed.load(), 0);
  FCR_CHECK_EQ(applied.load() + replayed.load(), kThreads);
  const fcr::Result<std::vector<fcr::PoolAccount>> pools = fixture.store.pools();
  FCR_REQUIRE(pools.has_value());
  FCR_CHECK_EQ((*pools)[0].committed, 5ULL);
}

FCR_TEST(concurrency, a_closed_store_is_reported_to_every_thread) {
  fcr_test::Fixture fixture = fcr_test::Fixture::make(kPools);
  FCR_REQUIRE_OK(fixture.store.reserve(fcr_test::reserve_request(
      fixture, "res-1", "attempt-1", {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)}, 1'100, 900'000)));
  fixture.store.close();

  std::atomic<int> closed_errors{0};
  std::atomic<int> other{0};
  std::vector<std::thread> workers;
  for (int thread = 0; thread < 4; ++thread) {
    workers.emplace_back([&fixture, &closed_errors, &other, thread] {
      for (int index = 0; index < 50; ++index) {
        const std::string id = "res-" + fcr::format_unsigned(static_cast<std::uint64_t>(thread)) + "-" +
                               fcr::format_unsigned(static_cast<std::uint64_t>(index));
        const fcr::Result<fcr::ReserveOutcome> outcome = fixture.store.reserve(fcr_test::reserve_request(
            fixture, id.c_str(), ("attempt-" + id).c_str(), {fcr_test::claim(fcr::ResourceKind::Rack, "hall-a", 1)},
            1'100, 900'000));
        if (outcome.has_value()) {
          continue;
        }
        if (outcome.error().code() == fcr::ErrorCode::StoreClosed) {
          closed_errors.fetch_add(1);
        } else {
          other.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  FCR_CHECK_EQ(closed_errors.load(), 200);
  FCR_CHECK_EQ(other.load(), 0);
  FCR_CHECK(fixture.store.closed());
}
