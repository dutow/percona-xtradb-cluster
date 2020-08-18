/*
  Copyright (c) 2019, 2020, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "mysql/harness/net_ts/buffer.h"

#include <list>

#include <gtest/gtest.h>

#if !defined(__SUNPRO_CC)
// devstudio 12.6 fails on x86_64 (on sparc, it passes) with:
//
// >> Assertion:   (../lnk/substitute.cc, line 1610)
//     while processing .../test_net_ts_buffer.cc at line 31.
static_assert(net::is_mutable_buffer_sequence<net::mutable_buffer>::value,
              "net::mutable_buffer MUST be a mutable_buffer_sequence");

static_assert(net::is_const_buffer_sequence<net::const_buffer>::value,
              "net::const_buffer MUST be a const_buffer_sequence");

static_assert(
    net::is_const_buffer_sequence<std::vector<net::const_buffer>>::value,
    "std::vector<net::const_buffer> MUST be a const_buffer_sequence");

static_assert(
    net::is_mutable_buffer_sequence<std::vector<net::mutable_buffer>>::value,
    "std::vector<net::mutable_buffer> MUST be a mutable_buffer_sequence");

static_assert(net::is_dynamic_buffer<net::dynamic_string_buffer<
                  std::string::value_type, std::string::traits_type,
                  std::string::allocator_type>>::value,
              "dynamic_string_buffer MUST be a dynamic-buffer");

static_assert(net::is_dynamic_buffer<net::dynamic_vector_buffer<
                  std::vector<uint8_t>::value_type,
                  std::vector<uint8_t>::allocator_type>>::value,
              "dynamic_vector_buffer MUST be a dynamic-buffer");

static_assert(
    net::is_const_buffer_sequence<
        net::prepared_buffers<net::const_buffer>>::value,
    "net::prepared_buffers<net::const_buffer> MUST be a const_buffer_sequence");
#endif

TEST(dynamic_string_bufffer, size_empty) {
  std::string s;
  auto sb = net::dynamic_buffer(s);

  EXPECT_EQ(sb.size(), s.size());
  EXPECT_EQ(sb.capacity(), s.capacity());
}

TEST(dynamic_string_bufffer, size_non_empty) {
  std::string s("aaaaaaaa");
  auto sb = net::dynamic_buffer(s);

  EXPECT_EQ(sb.size(), s.size());
  EXPECT_EQ(sb.capacity(), s.capacity());
}

// prepare() should lead to a resize of the buffer
TEST(dynamic_string_bufffer, prepare_from_empty) {
  std::string s;
  auto dyn_buf = net::dynamic_buffer(s);

  EXPECT_EQ(dyn_buf.size(), 0);

  auto b = dyn_buf.prepare(16);

  EXPECT_EQ(b.size(), 16);

  // another 16, but we havn't commit()ed the last one
  b = dyn_buf.prepare(16);

  EXPECT_EQ(b.size(), 16);
}

TEST(dynamic_string_bufffer, commit) {
  std::string s;
  auto dyn_buf = net::dynamic_buffer(s);

  EXPECT_EQ(dyn_buf.size(), 0);

  auto b = dyn_buf.prepare(16);

  // prepare should return a buffer of the expected size
  EXPECT_EQ(b.size(), 16);

  std::memset(b.data(), 'a', b.size());

  // commit, to move the buffer forward
  dyn_buf.commit(b.size());

  // underlying storage should have the expected content.
  EXPECT_STREQ(s.c_str(), "aaaaaaaaaaaaaaaa");

  SCOPED_TRACE("// prepare next block");

  b = dyn_buf.prepare(16);

  EXPECT_EQ(b.size(), 16);

  std::memset(b.data(), 'b', b.size());

  dyn_buf.commit(b.size());

  EXPECT_EQ(dyn_buf.size(), 32);
  EXPECT_EQ(s.size(), 32);

  EXPECT_STREQ(s.c_str(), "aaaaaaaaaaaaaaaabbbbbbbbbbbbbbbb");
}

// consume() always succeeds
TEST(dynamic_string_bufffer, consume_from_empty) {
  std::string s;
  auto dyn_buf = net::dynamic_buffer(s);
  EXPECT_EQ(dyn_buf.size(), 0);

  dyn_buf.consume(0);

  EXPECT_EQ(dyn_buf.size(), 0);
  dyn_buf.consume(16);

  EXPECT_EQ(dyn_buf.size(), 0);
}

TEST(dynamic_string_bufffer, consume_from_non_empty) {
  std::string s("aabb");
  auto dyn_buf = net::dynamic_buffer(s);

  EXPECT_EQ(dyn_buf.size(), 4);

  dyn_buf.consume(0);

  EXPECT_EQ(dyn_buf.size(), 4);
  dyn_buf.consume(2);

  EXPECT_EQ(dyn_buf.size(), 2);
  EXPECT_EQ(s.size(), 2);
  EXPECT_STREQ(s.c_str(), "bb");
  dyn_buf.consume(16);

  EXPECT_EQ(dyn_buf.size(), 0);
  EXPECT_EQ(s.size(), 0);
}

TEST(dynamic_string_bufffer, prepare_and_consume) {
  std::string s;
  auto dyn_buf = net::dynamic_buffer(s);

  // add 'aaaa' into the string
  auto b = dyn_buf.prepare(4);
  std::memset(b.data(), 'a', b.size());
  dyn_buf.commit(b.size());
  EXPECT_EQ(s, "aaaa");

  EXPECT_EQ(dyn_buf.size(), 4);
  b = dyn_buf.prepare(4);
  std::memset(b.data(), 'b', b.size());
  dyn_buf.commit(b.size());

  EXPECT_EQ(dyn_buf.size(), 8);
  EXPECT_EQ(s, "aaaabbbb");

  // consume 2 bytes
  dyn_buf.consume(2);
  EXPECT_EQ(dyn_buf.size(), 6);

  EXPECT_EQ(s, "aabbbb");

  // and append something again
  b = dyn_buf.prepare(2);
  std::memset(b.data(), 'a', b.size());
  dyn_buf.commit(b.size());

  EXPECT_EQ(dyn_buf.size(), 8);
  EXPECT_EQ(s, "aabbbbaa");
}

class ConsumingBuffers : public ::testing::Test {
 public:
  using T = std::list<std::string>;

  void SetUp() override {
    bufs.emplace_back("0123");
    bufs.emplace_back("45");
    bufs.emplace_back("6789");
  }

  void TearDown() override { bufs.clear(); }
  T bufs;
};

TEST_F(ConsumingBuffers, prepare_nothing) {
  net::consuming_buffers<T, net::const_buffer> buf_seq(bufs);

  // prepare nothing
  auto b = buf_seq.prepare(0);
  EXPECT_EQ(b.size(), 0);

  // nothing is consumed
  EXPECT_EQ(buf_seq.total_consumed(), 0);
}

TEST_F(ConsumingBuffers, prepare_one_buf) {
  net::consuming_buffers<T, net::const_buffer> buf_seq(bufs);

  // prepare something, which spans one buffer

  auto b = buf_seq.prepare(1);
  EXPECT_EQ(b.size(), 1);
  EXPECT_LE(b.size(), b.max_size());

  // nothing is consumed
  EXPECT_EQ(buf_seq.total_consumed(), 0);
}

TEST_F(ConsumingBuffers, prepare_two_buf) {
  net::consuming_buffers<T, net::const_buffer> buf_seq(bufs);

  // prepare something which spans 2 buffers

  auto b = buf_seq.prepare(5);
  EXPECT_EQ(b.size(), 2);
  EXPECT_LE(b.size(), b.max_size());

  // nothing is consumed
  EXPECT_EQ(buf_seq.total_consumed(), 0);
}

// prepare something which spans 3 buffers
TEST_F(ConsumingBuffers, prepare_3_buf) {
  net::consuming_buffers<T, net::const_buffer> buf_seq(bufs);

  auto b = buf_seq.prepare(7);
  EXPECT_EQ(b.size(), 3);
  EXPECT_LE(b.size(), b.max_size());

  // nothing is consumed
  EXPECT_EQ(buf_seq.total_consumed(), 0);
}

TEST_F(ConsumingBuffers, prepare_all) {
  net::consuming_buffers<T, net::const_buffer> buf_seq(bufs);

  // prepare all
  auto b = buf_seq.prepare(1024);
  EXPECT_EQ(b.size(), 3);
  EXPECT_LE(b.size(), b.max_size());
  buf_seq.consume(0);

  // nothing is consumed
  EXPECT_EQ(buf_seq.total_consumed(), 0);
}

TEST_F(ConsumingBuffers, consume_none) {
  net::consuming_buffers<T, net::const_buffer> buf_seq(bufs);

  buf_seq.consume(0);

  // nothing is consumed
  EXPECT_EQ(buf_seq.total_consumed(), 0);
}

TEST_F(ConsumingBuffers, consume_some_1) {
  net::consuming_buffers<T, net::const_buffer> buf_seq(bufs);

  // skip one
  buf_seq.consume(1);

  // prepare one
  auto prep_bufs = buf_seq.prepare(1);
  EXPECT_EQ(prep_bufs.size(), 1);

  auto cur = prep_bufs.begin();
  EXPECT_EQ(std::vector<char>(
                static_cast<const uint8_t *>(cur->data()),
                static_cast<const uint8_t *>(cur->data()) + cur->size()),
            (std::vector<char>{'1'}));

  EXPECT_EQ(buf_seq.total_consumed(), 1);
}

TEST_F(ConsumingBuffers, consume_some_2) {
  net::consuming_buffers<T, net::const_buffer> buf_seq(bufs);

  // skip one
  buf_seq.consume(1);

  // prepare something which spans 2 buffers
  auto prep_bufs = buf_seq.prepare(5);
  EXPECT_EQ(prep_bufs.size(), 2);

  auto cur = prep_bufs.begin();
  EXPECT_EQ(std::vector<char>(
                static_cast<const uint8_t *>(cur->data()),
                static_cast<const uint8_t *>(cur->data()) + cur->size()),
            (std::vector<char>{'1', '2', '3'}));

  std::advance(cur, 1);

  EXPECT_EQ(std::vector<char>(
                static_cast<const uint8_t *>(cur->data()),
                static_cast<const uint8_t *>(cur->data()) + cur->size()),
            (std::vector<char>{'4', '5'}));

  // prepare doesn't consume
  EXPECT_EQ(buf_seq.total_consumed(), 1);
}

TEST_F(ConsumingBuffers, consume_some_3) {
  net::consuming_buffers<T, net::const_buffer> buf_seq(bufs);

  // skip first block
  buf_seq.consume(4);

  // prepare something which spans 2 buffers
  auto prep_bufs = buf_seq.prepare(6);
  EXPECT_EQ(prep_bufs.size(), 2);

  auto cur = prep_bufs.begin();
  EXPECT_EQ(std::vector<char>(
                static_cast<const uint8_t *>(cur->data()),
                static_cast<const uint8_t *>(cur->data()) + cur->size()),
            (std::vector<char>{'4', '5'}));

  std::advance(cur, 1);

  EXPECT_EQ(std::vector<char>(
                static_cast<const uint8_t *>(cur->data()),
                static_cast<const uint8_t *>(cur->data()) + cur->size()),
            (std::vector<char>{'6', '7', '8', '9'}));

  // prepare doesn't consume
  EXPECT_EQ(buf_seq.total_consumed(), 4);
}

TEST_F(ConsumingBuffers, consume_some_all) {
  net::consuming_buffers<T, net::const_buffer> buf_seq(bufs);

  // consume all
  buf_seq.consume(10);

  // consumed more than we had
  EXPECT_EQ(buf_seq.total_consumed(), 10);
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
