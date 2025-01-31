////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 by EMC Corporation, All Rights Reserved
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is EMC Corporation
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "analysis/token_attributes.hpp"
#include "filter_test_case_base.hpp"
#include "formats/formats_10.hpp"
#include "search/same_position_filter.hpp"
#include "search/term_filter.hpp"
#include "store/memory_directory.hpp"
#include "tests_shared.hpp"

class same_position_filter_test_case : public tests::FilterTestCaseBase {
 protected:
  void sub_objects_ordered() {
    // add segment
    {
      tests::json_doc_generator gen(
        resource("phrase_sequential.json"),
        [](tests::document& doc, const std::string& name,
           const tests::json_doc_generator::json_value& data) {
          if (data.is_string()) {  // field
            doc.insert(
              std::make_shared<tests::text_field<std::string>>(name, data.str),
              true, false);
          } else if (data.is_number()) {  // seq
            const auto value = std::to_string(data.as_number<uint64_t>());
            doc.insert(std::make_shared<tests::string_field>(name, value),
                       false, true);
          }
        });
      add_segment(gen);
      gen.reset();
      add_segment(gen, irs::OM_APPEND);
    }

    // read segment
    auto index = open_reader();

    // collector count (no branches)
    {
      irs::by_same_position filter;

      size_t collect_field_count = 0;
      size_t collect_term_count = 0;
      size_t finish_count = 0;

      tests::sort::custom_sort scorer;

      scorer.collector_collect_field = [&collect_field_count](
                                         const irs::sub_reader&,
                                         const irs::term_reader&) -> void {
        ++collect_field_count;
      };
      scorer.collector_collect_term =
        [&collect_term_count](const irs::sub_reader&, const irs::term_reader&,
                              const irs::attribute_provider&) -> void {
        ++collect_term_count;
      };
      scorer.collectors_collect_ = [&finish_count](
                                     irs::byte_type*, const irs::index_reader&,
                                     const irs::sort::field_collector*,
                                     const irs::sort::term_collector*) -> void {
        ++finish_count;
      };
      scorer.prepare_field_collector_ =
        [&scorer]() -> irs::sort::field_collector::ptr {
        return std::make_unique<
          tests::sort::custom_sort::prepared::field_collector>(scorer);
      };
      scorer.prepare_term_collector_ =
        [&scorer]() -> irs::sort::term_collector::ptr {
        return std::make_unique<
          tests::sort::custom_sort::prepared::term_collector>(scorer);
      };

      auto pord = irs::Order::Prepare(scorer);
      auto prepared = filter.prepare(index, pord);
      ASSERT_EQ(0, collect_field_count);  // should not be executed
      ASSERT_EQ(0, collect_term_count);   // should not be executed
      ASSERT_EQ(0, finish_count);         // no terms optimization
    }

    // collector count (single term)
    {
      irs::by_same_position filter;
      filter.mutable_options()->terms.emplace_back(
        "phrase", irs::ViewCast<irs::byte_type>(std::string_view("quick")));

      size_t collect_field_count = 0;
      size_t collect_term_count = 0;
      size_t finish_count = 0;

      tests::sort::custom_sort scorer;

      scorer.collector_collect_field = [&collect_field_count](
                                         const irs::sub_reader&,
                                         const irs::term_reader&) -> void {
        ++collect_field_count;
      };
      scorer.collector_collect_term =
        [&collect_term_count](const irs::sub_reader&, const irs::term_reader&,
                              const irs::attribute_provider&) -> void {
        ++collect_term_count;
      };
      scorer.collectors_collect_ = [&finish_count](
                                     irs::byte_type*, const irs::index_reader&,
                                     const irs::sort::field_collector*,
                                     const irs::sort::term_collector*) -> void {
        ++finish_count;
      };
      scorer.prepare_field_collector_ =
        [&scorer]() -> irs::sort::field_collector::ptr {
        return std::make_unique<
          tests::sort::custom_sort::prepared::field_collector>(scorer);
      };
      scorer.prepare_term_collector_ =
        [&scorer]() -> irs::sort::term_collector::ptr {
        return std::make_unique<
          tests::sort::custom_sort::prepared::term_collector>(scorer);
      };

      auto pord = irs::Order::Prepare(scorer);
      auto prepared = filter.prepare(index, pord);
      ASSERT_EQ(2, collect_field_count);  // 1 field in 2 segments
      ASSERT_EQ(2, collect_term_count);   // 1 term in 2 segments
      ASSERT_EQ(1, finish_count);         // 1 unique term
    }

    // collector count (multiple terms)
    {
      irs::by_same_position filter;
      filter.mutable_options()->terms.emplace_back(
        "phrase", irs::ViewCast<irs::byte_type>(std::string_view("quick")));
      filter.mutable_options()->terms.emplace_back(
        "phrase", irs::ViewCast<irs::byte_type>(std::string_view("brown")));

      size_t collect_field_count = 0;
      size_t collect_term_count = 0;
      size_t finish_count = 0;

      tests::sort::custom_sort scorer;

      scorer.collector_collect_field = [&collect_field_count](
                                         const irs::sub_reader&,
                                         const irs::term_reader&) -> void {
        ++collect_field_count;
      };
      scorer.collector_collect_term =
        [&collect_term_count](const irs::sub_reader&, const irs::term_reader&,
                              const irs::attribute_provider&) -> void {
        ++collect_term_count;
      };
      scorer.collectors_collect_ = [&finish_count](
                                     irs::byte_type*, const irs::index_reader&,
                                     const irs::sort::field_collector*,
                                     const irs::sort::term_collector*) -> void {
        ++finish_count;
      };
      scorer.prepare_field_collector_ =
        [&scorer]() -> irs::sort::field_collector::ptr {
        return std::make_unique<
          tests::sort::custom_sort::prepared::field_collector>(scorer);
      };
      scorer.prepare_term_collector_ =
        [&scorer]() -> irs::sort::term_collector::ptr {
        return std::make_unique<
          tests::sort::custom_sort::prepared::term_collector>(scorer);
      };

      auto pord = irs::Order::Prepare(scorer);
      auto prepared = filter.prepare(index, pord);
      ASSERT_EQ(4, collect_field_count);  // 2 fields (1 per term since treated
                                          // as a disjunction) in 2 segments
      ASSERT_EQ(4, collect_term_count);   // 2 term in 2 segments
      ASSERT_EQ(2, finish_count);         // 2 unique terms
    }
  }

  void sub_objects_unordered() {
    // add segment
    tests::json_doc_generator gen(
      resource("same_position.json"),
      [](tests::document& doc, const std::string& name,
         const tests::json_doc_generator::json_value& data) {
        typedef tests::text_field<std::string> text_field;
        if (data.is_string()) {
          // a || b || c
          doc.indexed.push_back(std::make_shared<text_field>(name, data.str));
        } else if (data.is_number()) {
          // _id
          const auto lValue = data.as_number<uint64_t>();

          // 'value' can be interpreted as a double
          doc.insert(std::make_shared<tests::long_field>());
          auto& field = (doc.indexed.end() - 1).as<tests::long_field>();
          field.name(name);
          field.value(lValue);
        }
      });
    add_segment(gen);

    // read segment
    auto index = open_reader();
    ASSERT_EQ(1, index.size());
    auto& segment = *(index.begin());

    irs::bytes_view_input in;
    auto column = segment.column("_id");
    ASSERT_NE(nullptr, column);

    // empty query
    {
      irs::by_same_position q;
      auto prepared = q.prepare(index);
      auto docs = prepared->execute(segment);
      auto* doc = irs::get<irs::document>(*docs);
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(docs->value(), doc->value);
      ASSERT_FALSE(docs->next());
      ASSERT_EQ(docs->value(), doc->value);
    }

    // { a: 100 } - equal to 'by_term'
    {
      irs::by_same_position query;
      query.mutable_options()->terms.emplace_back(
        "a", irs::ViewCast<irs::byte_type>(std::string_view("100")));

      irs::by_term expected_query;
      *expected_query.mutable_field() = "a";
      expected_query.mutable_options()->term =
        irs::ViewCast<irs::byte_type>(std::string_view("100"));

      auto prepared = query.prepare(index);
      auto expected_prepared = expected_query.prepare(index);

      auto docs = prepared->execute(segment);
      auto* doc = irs::get<irs::document>(*docs);
      ASSERT_TRUE(bool(doc));
      ASSERT_EQ(docs->value(), doc->value);
      auto expected_docs = prepared->execute(segment);

      ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(),
                docs->value());
      while (expected_docs->next()) {
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(expected_docs->value(), docs->value());
        ASSERT_EQ(docs->value(), doc->value);
      }
      ASSERT_FALSE(docs->next());
      ASSERT_EQ(irs::doc_limits::eof(), docs->value());
    }

    // check document with first position
    // { a: 300, b:90, c:9 }
    {
      irs::by_same_position q;
      q.mutable_options()->terms.emplace_back(
        "a", irs::ViewCast<irs::byte_type>(std::string_view("300")));
      q.mutable_options()->terms.emplace_back(
        "b", irs::ViewCast<irs::byte_type>(std::string_view("90")));
      q.mutable_options()->terms.emplace_back(
        "c", irs::ViewCast<irs::byte_type>(std::string_view("9")));
      auto prepared = q.prepare(index);
      auto docs = prepared->execute(segment);
      auto* doc = irs::get<irs::document>(*docs);
      ASSERT_EQ(docs->value(), doc->value);
      ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(),
                docs->value());
      ASSERT_TRUE(docs->next());
      ASSERT_EQ(1, docs->value());
    }

    // { a: 100, b:30, c:6 }
    {
      irs::by_same_position q;
      q.mutable_options()->terms.emplace_back(
        "a", irs::ViewCast<irs::byte_type>(std::string_view("100")));
      q.mutable_options()->terms.emplace_back(
        "b", irs::ViewCast<irs::byte_type>(std::string_view("30")));
      q.mutable_options()->terms.emplace_back(
        "c", irs::ViewCast<irs::byte_type>(std::string_view("6")));

      auto prepared = q.prepare(index);

      // next
      {
        auto values = column->iterator(irs::ColumnHint::kNormal);
        ASSERT_NE(nullptr, values);
        auto* actual_value = irs::get<irs::payload>(*values);
        ASSERT_NE(nullptr, actual_value);

        auto docs = prepared->execute(segment);
        auto* doc = irs::get<irs::document>(*docs);
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(docs->value(), doc->value);
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(),
                  docs->value());
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(6, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(27, irs::read_zvlong(in));
        ASSERT_FALSE(docs->next());
        ASSERT_EQ(irs::doc_limits::eof(), docs->value());
      }

      // seek
      {
        auto values = column->iterator(irs::ColumnHint::kNormal);
        ASSERT_NE(nullptr, values);
        auto* actual_value = irs::get<irs::payload>(*values);
        ASSERT_NE(nullptr, actual_value);

        auto docs = prepared->execute(segment);
        auto* doc = irs::get<irs::document>(*docs);
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(docs->value(), doc->value);
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(),
                  docs->value());
        ASSERT_EQ((irs::type_limits<irs::type_t::doc_id_t>::min)() + 6,
                  docs->seek((irs::type_limits<irs::type_t::doc_id_t>::min)()));
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(6, irs::read_zvlong(in));
        ASSERT_EQ((irs::type_limits<irs::type_t::doc_id_t>::min)() + 27,
                  docs->seek(27));
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(27, irs::read_zvlong(in));
        ASSERT_EQ((irs::type_limits<irs::type_t::doc_id_t>::min)() + 27,
                  docs->seek(8));  // seek backwards
        ASSERT_EQ((irs::type_limits<irs::type_t::doc_id_t>::min)() + 27,
                  docs->seek(27));  // seek to same position
        ASSERT_FALSE(docs->next());
        ASSERT_EQ(irs::doc_limits::eof(), docs->value());
      }
    }

    // { c: 8, b:80, a:700 }
    {
      irs::by_same_position q;
      q.mutable_options()->terms.emplace_back(
        "c", irs::ViewCast<irs::byte_type>(std::string_view("8")));
      q.mutable_options()->terms.emplace_back(
        "b", irs::ViewCast<irs::byte_type>(std::string_view("80")));
      q.mutable_options()->terms.emplace_back(
        "a", irs::ViewCast<irs::byte_type>(std::string_view("700")));

      auto prepared = q.prepare(index);

      // next
      {
        auto values = column->iterator(irs::ColumnHint::kNormal);
        ASSERT_NE(nullptr, values);
        auto* actual_value = irs::get<irs::payload>(*values);
        ASSERT_NE(nullptr, actual_value);

        auto docs = prepared->execute(segment);
        auto* doc = irs::get<irs::document>(*docs);
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(docs->value(), doc->value);
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(),
                  docs->value());
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(14, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(91, irs::read_zvlong(in));
        ASSERT_FALSE(docs->next());
        ASSERT_EQ(irs::doc_limits::eof(), docs->value());
      }

      // seek
      {
        auto values = column->iterator(irs::ColumnHint::kNormal);
        ASSERT_NE(nullptr, values);
        auto* actual_value = irs::get<irs::payload>(*values);
        ASSERT_NE(nullptr, actual_value);

        auto docs = prepared->execute(segment);
        auto* doc = irs::get<irs::document>(*docs);
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(docs->value(), doc->value);
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(),
                  docs->value());
        ASSERT_EQ((irs::type_limits<irs::type_t::doc_id_t>::min)() + 91,
                  docs->seek(27));
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(91, irs::read_zvlong(in));
        ASSERT_EQ((irs::type_limits<irs::type_t::doc_id_t>::min)() + 91,
                  docs->seek(8));  // seek backwards
        ASSERT_EQ((irs::type_limits<irs::type_t::doc_id_t>::min)() + 91,
                  docs->seek(27));  // seek to same position
        ASSERT_FALSE(docs->next());
        ASSERT_EQ(irs::doc_limits::eof(), docs->value());
      }
    }

    // { a: 700, b:*, c: 7 }
    {
      irs::by_same_position q;
      q.mutable_options()->terms.emplace_back(
        "a", irs::ViewCast<irs::byte_type>(std::string_view("700")));
      q.mutable_options()->terms.emplace_back(
        "c", irs::ViewCast<irs::byte_type>(std::string_view("7")));

      auto prepared = q.prepare(index);

      // next
      {
        auto values = column->iterator(irs::ColumnHint::kNormal);
        ASSERT_NE(nullptr, values);
        auto* actual_value = irs::get<irs::payload>(*values);
        ASSERT_NE(nullptr, actual_value);

        auto docs = prepared->execute(segment);
        auto* doc = irs::get<irs::document>(*docs);
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(docs->value(), doc->value);
        ASSERT_EQ(irs::type_limits<irs::type_t::doc_id_t>::invalid(),
                  docs->value());
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(1, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(6, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(11, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(17, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(18, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(23, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(24, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(28, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(38, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(51, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(66, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(79, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(89, irs::read_zvlong(in));
        ASSERT_FALSE(docs->next());
        ASSERT_EQ(irs::doc_limits::eof(), docs->value());
      }

      // seek + next
      {
        auto values = column->iterator(irs::ColumnHint::kNormal);
        ASSERT_NE(nullptr, values);
        auto* actual_value = irs::get<irs::payload>(*values);
        ASSERT_NE(nullptr, actual_value);

        auto docs = prepared->execute(segment);
        auto* doc = irs::get<irs::document>(*docs);
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(docs->value(), doc->value);
        ASSERT_EQ(irs::doc_limits::invalid(), docs->value());
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(1, irs::read_zvlong(in));
        ASSERT_EQ((irs::doc_limits::min)() + 28,
                  docs->seek((irs::doc_limits::min)() + 28));
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(28, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(38, irs::read_zvlong(in));
        ASSERT_EQ((irs::doc_limits::min)() + 51, docs->seek(45));
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(51, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(66, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(79, irs::read_zvlong(in));
        ASSERT_TRUE(docs->next());
        ASSERT_EQ(docs->value(), values->seek(docs->value()));
        in.reset(actual_value->value);
        ASSERT_EQ(89, irs::read_zvlong(in));
        ASSERT_FALSE(docs->next());
        ASSERT_EQ(irs::doc_limits::eof(), docs->value());
      }

      // seek to the end
      {
        auto docs = prepared->execute(segment);
        auto* doc = irs::get<irs::document>(*docs);
        ASSERT_TRUE(bool(doc));
        ASSERT_EQ(docs->value(), doc->value);
        ASSERT_EQ(irs::doc_limits::invalid(), docs->value());
        ASSERT_EQ(irs::doc_limits::eof(), docs->seek(irs::doc_limits::eof()));
        ASSERT_FALSE(docs->next());
        ASSERT_EQ(irs::doc_limits::eof(), docs->value());
      }
    }
  }
};  // same_position_filter_test_case

TEST_P(same_position_filter_test_case, by_same_position) {
  sub_objects_ordered();
  sub_objects_unordered();
}

// ----------------------------------------------------------------------------
// --SECTION--                                      by_same_position base tests
// ----------------------------------------------------------------------------

TEST(by_same_position_test, options) {
  irs::by_same_position_options opts;
  ASSERT_TRUE(opts.terms.empty());
}

TEST(by_same_position_test, ctor) {
  irs::by_same_position q;
  ASSERT_EQ(irs::type<irs::by_same_position>::id(), q.type());
  ASSERT_EQ(irs::by_same_position_options{}, q.options());
  ASSERT_EQ(irs::kNoBoost, q.boost());

  static_assert((irs::IndexFeatures::FREQ | irs::IndexFeatures::POS) ==
                irs::by_same_position::kRequiredFeatures);
}

TEST(by_same_position_test, boost) {
  // no boost
  {
    (void)1;  // format work-around
    // no branches
    {
      irs::by_same_position q;

      auto prepared = q.prepare(irs::sub_reader::empty());
      ASSERT_EQ(irs::kNoBoost, prepared->boost());
    }

    // single term
    {
      irs::by_same_position q;
      q.mutable_options()->terms.emplace_back(
        "field", irs::ViewCast<irs::byte_type>(std::string_view("quick")));

      auto prepared = q.prepare(irs::sub_reader::empty());
      ASSERT_EQ(irs::kNoBoost, prepared->boost());
    }

    // multiple terms
    {
      irs::by_same_position q;
      q.mutable_options()->terms.emplace_back(
        "field", irs::ViewCast<irs::byte_type>(std::string_view("quick")));
      q.mutable_options()->terms.emplace_back(
        "field", irs::ViewCast<irs::byte_type>(std::string_view("brown")));

      auto prepared = q.prepare(irs::sub_reader::empty());
      ASSERT_EQ(irs::kNoBoost, prepared->boost());
    }
  }

  // with boost
  {
    irs::score_t boost = 1.5f;

    // no terms, return empty query
    {
      irs::by_same_position q;
      q.boost(boost);

      auto prepared = q.prepare(irs::sub_reader::empty());
      ASSERT_EQ(irs::kNoBoost, prepared->boost());
    }

    // single term
    {
      irs::by_same_position q;
      q.mutable_options()->terms.emplace_back(
        "field", irs::ViewCast<irs::byte_type>(std::string_view("quick")));
      q.boost(boost);

      auto prepared = q.prepare(irs::sub_reader::empty());
      ASSERT_EQ(boost, prepared->boost());
    }

    // single multiple terms
    {
      irs::by_same_position q;
      q.mutable_options()->terms.emplace_back(
        "field", irs::ViewCast<irs::byte_type>(std::string_view("quick")));
      q.mutable_options()->terms.emplace_back(
        "field", irs::ViewCast<irs::byte_type>(std::string_view("brown")));
      q.boost(boost);

      auto prepared = q.prepare(irs::sub_reader::empty());
      ASSERT_EQ(boost, prepared->boost());
    }
  }
}

TEST(by_same_position_test, equal) {
  ASSERT_EQ(irs::by_same_position(), irs::by_same_position());

  {
    irs::by_same_position q0;
    q0.mutable_options()->terms.emplace_back(
      "speed", irs::ViewCast<irs::byte_type>(std::string_view("quick")));
    q0.mutable_options()->terms.emplace_back(
      "color", irs::ViewCast<irs::byte_type>(std::string_view("brown")));

    irs::by_same_position q1;
    q1.mutable_options()->terms.emplace_back(
      "speed", irs::ViewCast<irs::byte_type>(std::string_view("quick")));
    q1.mutable_options()->terms.emplace_back(
      "color", irs::ViewCast<irs::byte_type>(std::string_view("brown")));
    ASSERT_EQ(q0, q1);
  }

  {
    irs::by_same_position q0;
    q0.mutable_options()->terms.emplace_back(
      "speed", irs::ViewCast<irs::byte_type>(std::string_view("quick")));
    q0.mutable_options()->terms.emplace_back(
      "color", irs::ViewCast<irs::byte_type>(std::string_view("brown")));
    q0.mutable_options()->terms.emplace_back(
      "name", irs::ViewCast<irs::byte_type>(std::string_view("fox")));

    irs::by_same_position q1;
    q1.mutable_options()->terms.emplace_back(
      "speed", irs::ViewCast<irs::byte_type>(std::string_view("quick")));
    q1.mutable_options()->terms.emplace_back(
      "color", irs::ViewCast<irs::byte_type>(std::string_view("brown")));
    q1.mutable_options()->terms.emplace_back(
      "name", irs::ViewCast<irs::byte_type>(std::string_view("squirrel")));
    ASSERT_NE(q0, q1);
  }

  {
    irs::by_same_position q0;
    q0.mutable_options()->terms.emplace_back(
      "Speed", irs::ViewCast<irs::byte_type>(std::string_view("quick")));
    q0.mutable_options()->terms.emplace_back(
      "color", irs::ViewCast<irs::byte_type>(std::string_view("brown")));
    q0.mutable_options()->terms.emplace_back(
      "name", irs::ViewCast<irs::byte_type>(std::string_view("fox")));

    irs::by_same_position q1;
    q1.mutable_options()->terms.emplace_back(
      "speed", irs::ViewCast<irs::byte_type>(std::string_view("quick")));
    q1.mutable_options()->terms.emplace_back(
      "color", irs::ViewCast<irs::byte_type>(std::string_view("brown")));
    q1.mutable_options()->terms.emplace_back(
      "name", irs::ViewCast<irs::byte_type>(std::string_view("fox")));
    ASSERT_NE(q0, q1);
  }

  {
    irs::by_same_position q0;
    q0.mutable_options()->terms.emplace_back(
      "speed", irs::ViewCast<irs::byte_type>(std::string_view("quick")));
    q0.mutable_options()->terms.emplace_back(
      "color", irs::ViewCast<irs::byte_type>(std::string_view("brown")));

    irs::by_same_position q1;
    q1.mutable_options()->terms.emplace_back(
      "speed", irs::ViewCast<irs::byte_type>(std::string_view("quick")));
    q1.mutable_options()->terms.emplace_back(
      "color", irs::ViewCast<irs::byte_type>(std::string_view("brown")));
    q1.mutable_options()->terms.emplace_back(
      "name", irs::ViewCast<irs::byte_type>(std::string_view("fox")));
    ASSERT_NE(q0, q1);
  }
}

INSTANTIATE_TEST_SUITE_P(
  same_position_filter_test, same_position_filter_test_case,
  ::testing::Combine(
    ::testing::Values(&tests::directory<&tests::memory_directory>,
                      &tests::directory<&tests::fs_directory>,
                      &tests::directory<&tests::mmap_directory>),
    ::testing::Values(tests::format_info{"1_0"},
                      tests::format_info{"1_1", "1_0"},
                      tests::format_info{"1_3", "1_0"})),
  same_position_filter_test_case::to_string);
