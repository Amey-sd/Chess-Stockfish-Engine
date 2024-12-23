/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2013 Marco Costalba, Joona Kiiski, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cassert>
#include <sstream>

#include "evaluate.h"
#include "misc.h"
#include "thread.h"
#include "tt.h"
#include "ucioption.h"

using std::string;

UCI::OptionsMap Options; // Global object

namespace UCI {

/// 'On change' actions, triggered by an option's value change
void on_logger(const Option& o) { start_logger(o); }
void on_eval(const Option&) { Eval::init(); }
void on_threads(const Option&) { Threads.read_uci_options(); }
void on_hash_size(const Option& o) { TT.set_size(o); }
void on_clear_hash(const Option&) { TT.clear(); }


/// Our case insensitive less() function as required by UCI protocol
bool CaseInsensitiveLess::operator() (const string& s1, const string& s2) const {

  return std::lexicographical_compare(s1.begin(), s1.end(), s2.begin(), s2.end(),
         [](char c1, char c2) { return tolower(c1) < tolower(c2); });
}


/// init() initializes the UCI options to their hard coded default values

void init(OptionsMap& o) {

  o["Write Debug Log"]             = Option(false, on_logger);
  o["Write Search Log"]            = Option(false);
  o["Search Log Filename"]         = Option("SearchLog.txt");
  o["Book File"]                   = Option("book.bin");
  o["Best Book Move"]              = Option(false);
  o["Contempt Factor"]             = Option(0, -50,  50);
  o["Mobility (Midgame)"]          = Option(100, 0, 200, on_eval);
  o["Mobility (Endgame)"]          = Option(100, 0, 200, on_eval);
  o["Pawn Structure (Midgame)"]    = Option(100, 0, 200, on_eval);
  o["Pawn Structure (Endgame)"]    = Option(100, 0, 200, on_eval);
  o["Passed Pawns (Midgame)"]      = Option(100, 0, 200, on_eval);
  o["Passed Pawns (Endgame)"]      = Option(100, 0, 200, on_eval);
  o["Space"]                       = Option(100, 0, 200, on_eval);
  o["Aggressiveness"]              = Option(100, 0, 200, on_eval);
  o["Cowardice"]                   = Option(100, 0, 200, on_eval);
  o["Min Split Depth"]             = Option(0, 0, 12, on_threads);
  o["Max Threads per Split Point"] = Option(5, 4,  8, on_threads);
  o["Threads"]                     = Option(1, 1, MAX_THREADS, on_threads);
  o["Idle Threads Sleep"]          = Option(false);
  o["Hash"]                        = Option(32, 1, 8192, on_hash_size);
  o["Clear Hash"]                  = Option(on_clear_hash);
  o["Ponder"]                      = Option(true);
  o["OwnBook"]                     = Option(false);
  o["MultiPV"]                     = Option(1, 1, 500);
  o["Skill Level"]                 = Option(20, 0, 20);
  o["Emergency Move Horizon"]      = Option(40, 0, 50);
  o["Emergency Base Time"]         = Option(200, 0, 30000);
  o["Emergency Move Time"]         = Option(70, 0, 5000);
  o["Minimum Thinking Time"]       = Option(20, 0, 5000);
  o["Slow Mover"]                  = Option(100, 10, 1000);
  o["UCI_Chess960"]                = Option(false);
  o["UCI_AnalyseMode"]             = Option(false, on_eval);
}


/// operator<<() is used to print all the options default values in chronological
/// insertion order (the idx field) and in the format defined by the UCI protocol.

std::ostream& operator<<(std::ostream& os, const OptionsMap& om) {

  for (size_t idx = 0; idx < om.size(); idx++)
  {
      auto it = std::find_if(om.begin(), om.end(), [idx](const OptionsMap::value_type& p)
                                                   { return p.second.idx == idx; });
      const Option& o = it->second;
      os << "\noption name " << it->first << " type " << o.type;

      if (o.type != "button")
          os << " default " << o.defaultValue;

      if (o.type == "spin")
          os << " min " << o.min << " max " << o.max;
  }
  return os;
}


/// Option c'tors and conversion operators

Option::Option(const char* v, Fn* f) : type("string"), min(0), max(0), idx(Options.size()), on_change(f)
{ defaultValue = currentValue = v; }

Option::Option(bool v, Fn* f) : type("check"), min(0), max(0), idx(Options.size()), on_change(f)
{ defaultValue = currentValue = (v ? "true" : "false"); }

Option::Option(Fn* f) : type("button"), min(0), max(0), idx(Options.size()), on_change(f)
{}

Option::Option(int v, int minv, int maxv, Fn* f) : type("spin"), min(minv), max(maxv), idx(Options.size()), on_change(f)
{ defaultValue = currentValue = std::to_string(v); }


Option::operator int() const {
  assert(type == "check" || type == "spin");
  return (type == "spin" ? stoi(currentValue) : currentValue == "true");
}

Option::operator std::string() const {
  assert(type == "string");
  return currentValue;
}


/// operator=() updates currentValue and triggers on_change() action. It's up to
/// the GUI to check for option's limits, but we could receive the new value from
/// the user by console window, so let's check the bounds anyway.

Option& Option::operator=(const string& v) {

  assert(!type.empty());

  if (   (type != "button" && v.empty())
      || (type == "check" && v != "true" && v != "false")
      || (type == "spin" && (stoi(v) < min || stoi(v) > max)))
      return *this;

  if (type != "button")
      currentValue = v;

  if (on_change)
      (*on_change)(*this);

  return *this;
}

} // namespace UCI
