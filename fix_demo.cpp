#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <streambuf>
#include <string>
#include <tuple>
#include <typeinfo>
#include <utility>
#include <variant>

/* This guy is going to allow streams to automatically delimit input on FIX "tagvalue" separators ('=') and FIX "tagfield" delimiters ('|').
*/
class fix_whitespace : public std::ctype<char> {
	const mask* derive_from_table(const mask* table) {
		static mask v[table_size];
		std::copy(table, table + table_size, v);

		v['|'] |= std::ctype_base::space;
		v['='] |= std::ctype_base::space;

		return v;
	}

public:
	fix_whitespace(const mask* table) : ctype<char>{ derive_from_table(table), false, 0 } {}
};

enum class field_tag : int { Account = 1, MsgType = 35, Price = 44 };

std::istream& operator>>(std::istream& is, field_tag& tn) {
	// The iterator check is because stream iterators can detach upon construction, and dereferencing a detached stream iterator is UB.
	if (auto iter = std::istream_iterator<std::underlying_type_t<field_tag>>{ is }; iter != std::istream_iterator<std::underlying_type_t<field_tag>>{}) {
		if (const auto in = *iter; iter != std::istream_iterator<std::underlying_type_t<field_tag>>{}) {
			tn = static_cast<field_tag>(in);
		}
	}

	return is;
}

// Used for the set I use to keep track of duplicate field errors.
bool operator <(field_tag l, field_tag r) { return static_cast<std::underlying_type_t<field_tag>>(l) < static_cast<std::underlying_type_t<field_tag>>(r); }

class tagvalue {
	std::istream* isp; //"If I had more time, I would have written a shorter letter." - paraphrased from Mark Twain
	field_tag tag;

	friend std::istream& operator >>(std::istream& is, tagvalue& tv) {
		if (is >> tv.tag) {
			tv.isp = &is;
		}

		return is;
	}

public:
	explicit operator field_tag() const { return tag; }

	explicit operator long double() const {
		if (auto iter = std::istream_iterator<long double>{ *isp }; iter != std::istream_iterator<long double>{}) {
			if (auto ld = *iter; iter != std::istream_iterator<long double>{}) {
				return ld;
			}
		}

		throw std::bad_cast();
	}

	explicit operator std::string() const {
		if (auto iter = std::istream_iterator<std::string>{ *isp }; iter != std::istream_iterator<std::string>{}) {
			if (auto s = *iter; iter != std::istream_iterator<std::string>{}) {
				return s;
			}
		}

		throw std::bad_cast();
	}
};

struct error {};
struct purge {};

// I'm only using long double for currency because that's what `std::get_money` uses, which is how I started out.
using production = std::variant<std::monostate, std::tuple<std::string, long double>, error, purge>;

// This is a functor we're going to stick into `std::transform`.
struct tagvalue_processor {
	std::set<field_tag> discovered_fields;
	std::optional<std::string> account;
	std::optional<long double> price;
	std::istream& is;
	bool message_is_new_order_single;

	bool is_a_duplicate(const field_tag& ft) { return !std::get<bool>(discovered_fields.insert(ft)); }

	void ignore_the_rest_of_the_message() {
		is.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		discovered_fields.clear();
	}

	void ignore_the_field() {
		is.ignore(std::numeric_limits<std::streamsize>::max(), '|');
	}

	void reset() {
		message_is_new_order_single = false;
		account.reset();
		price.reset();
		discovered_fields.clear();
	}

	// We're going to process over field IDs, mostly, and occasionally cache a field value.
	production operator()(const tagvalue& tv) {
		production ret_val;
		if (is_a_duplicate(static_cast<field_tag>(tv))) {
			// Since we know we're hosed, we can just report the error and move on.
			ret_val = error{};
			ignore_the_rest_of_the_message();
			reset();
		}
		else switch (static_cast<field_tag>(tv)) {
		case field_tag::Account: account = static_cast<std::string>(tv); break;
		case field_tag::MsgType: if ("D" == static_cast<std::string>(tv)) { message_is_new_order_single = true; } break;
		case field_tag::Price: price = static_cast<long double>(tv);  break;
		default: ignore_the_field(); break;
		}

		if (is.peek() == std::char_traits<char>::to_int_type('\n')) {
			is.ignore();
			if (message_is_new_order_single && account && price) {
				// At this point we know we've avoided duplicates.
				// Duplicate handling purges us of the newline.
				ret_val = std::make_tuple(*account, *price);
			}
			else {
				ret_val = purge{};
			}

			reset();
		}

		return ret_val;
	}

	tagvalue_processor(std::istream& is) :is{ is }, message_is_new_order_single{ false } {
	}
};

template<class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

using map_of_high_low_per_account = std::map<std::string, std::tuple<std::optional<long double>, std::optional<long double>>>;

// If it looks like an output iterator, acts like an output iterator...
struct sink {
	std::reference_wrapper<std::istream> ss;
	map_of_high_low_per_account mp;

	sink& operator *() { return *this; }
	sink& operator++() { return *this; }
	sink operator++(int) { return *this; }

	sink& operator =(const production& p) {
		auto& mp = this->mp; //This is due to an apparent issue with the MS compiler.
		auto& ss = this->ss.get();
		std::visit(overloaded{
			[](const std::monostate) {}, // no-op.
			[&ss](const error) {
				std::cerr << ss.rdbuf();
				ss.seekg(0, std::ios_base::end);
			}, // Signaled an error.
			[&ss](const purge) {
				ss.seekg(0, std::ios_base::end);
			},
			[&mp, &ss](const std::tuple<std::string, long double>& data) { // Got one.
				auto& [high, low] = mp[std::get<std::string>(data)];

				// A single order in the input is both the high and the low.
				if (!high || *high < std::get<long double>(data)) {
					high = std::get<long double>(data);
				}

				if (!low || *low > std::get<long double>(data)) {
					low = std::get<long double>(data);
				}
				ss.seekg(0, std::ios_base::end);
			}
			}, p);

		return *this;
	}
};

class tappedstreambuf : public std::streambuf {
	std::streambuf* src, * dst;

	// Gets called by sbumpc and consumes the character
	int_type uflow() override {
		if (traits_type::eq_int_type(src->sgetc(), traits_type::eof())) {
			return traits_type::eof();
		}

		return dst->sputc(traits_type::to_char_type(src->sbumpc()));
	}

	int_type underflow() override { return src->sgetc(); }

public:
	tappedstreambuf(std::streambuf* src, std::streambuf* dst) : src{ src }, dst{ dst } {}
};

// A convenient wrapper.
class tappedistream : public std::istream {
	tappedstreambuf tb;

public:

	tappedistream(std::istream& src, std::istream& dst) : std::istream{ &tb }, tb{ src.rdbuf(), dst.rdbuf() } {}
};

int main() {
	std::ios_base::sync_with_stdio(false);
	std::cin.tie(nullptr);

	std::stringstream error_message_stream;
	tappedistream tapped_istream{ std::cin, error_message_stream };
	tagvalue_processor tvp{ tapped_istream };
	tapped_istream.imbue(std::locale(tapped_istream.getloc(), new fix_whitespace{ std::use_facet<std::ctype<char>>(tapped_istream.getloc()).table() }));

	auto s = std::transform(std::istream_iterator<tagvalue>{tapped_istream}, {}, sink{ error_message_stream }, tvp);

	std::cout << "High/Low Report:\n" << std::left << std::setw(12) << "Account" << std::setw(8) << "High" << std::setw(8) << "Low\n"
		<< std::setfill('-') << std::setw(28) << "" << std::setfill(' ') << '\n';

	std::ranges::for_each(s.mp, [](const auto& kvp) {
		const auto& [account, highlow] = kvp;
		const auto& [high, low] = highlow;
		std::cout << std::setw(12) << account << std::setw(8) << *high << std::setw(8) << *low << '\n';
		});

	return std::cout << std::flush && std::cin.eof() ? EXIT_SUCCESS : EXIT_FAILURE;
}
