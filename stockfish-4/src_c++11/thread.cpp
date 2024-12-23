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

#include <algorithm> // For std::count
#include <cassert>

#include "movegen.h"
#include "search.h"
#include "thread.h"
#include "ucioption.h"

using namespace Search;

ThreadPool Threads; // Global object

namespace {

 // Helpers to launch a thread after creation and joining before delete. Must be
 // outside Thread c'tor and d'tor because object shall be fully initialized
 // when virtual idle_loop() is called and when joining.
 template<typename T> T* new_thread() {
   T* th = new T();
   th->nativeThread = std::thread(&ThreadBase::idle_loop, th); // Will go to sleep
   return th;
 }

 void delete_thread(ThreadBase* th) {
   th->exit = true; // Search must be already finished
   th->notify_one();
   th->nativeThread.join(); // Wait for thread termination
   delete th;
 }

}

// ThreadBase::notify_one() wakes up the thread when there is some search to do

void ThreadBase::notify_one() {

  std::unique_lock<std::mutex>(this->mutex);
  sleepCondition.notify_one();
}


// ThreadBase::wait_for() set the thread to sleep until condition 'b' turns true

void ThreadBase::wait_for(volatile const bool& b) {

  std::unique_lock<std::mutex> lk(mutex);
  sleepCondition.wait(lk, [&]{ return b; });
}


// Thread c'tor just inits data but does not launch any thread of execution that
// instead will be started only upon c'tor returns.

Thread::Thread() /* : splitPoints() */ { // Value-initialization bug in MSVC

  searching = false;
  maxPly = splitPointsSize = 0;
  activeSplitPoint = nullptr;
  activePosition = nullptr;
  idx = Threads.size();
}


// TimerThread::idle_loop() is where the timer thread waits msec milliseconds
// and then calls check_time(). If msec is 0 thread sleeps until is woken up.
extern void check_time();

void TimerThread::idle_loop() {

  while (!exit)
  {
      std::unique_lock<std::mutex> lk(mutex);

      if (!exit)
          sleepCondition.wait_for(lk, std::chrono::milliseconds(msec ? msec : INT_MAX));

      lk.unlock();

      if (msec)
          check_time();
  }
}


// MainThread::idle_loop() is where the main thread is parked waiting to be started
// when there is a new search. Main thread will launch all the slave threads.

void MainThread::idle_loop() {

  while (true)
  {
      std::unique_lock<std::mutex> lk(mutex);

      thinking = false;

      while (!thinking && !exit)
      {
          Threads.sleepCondition.notify_one(); // Wake up UI thread if needed
          sleepCondition.wait(lk);
      }

      lk.unlock();

      if (exit)
          return;

      searching = true;

      Search::think();

      assert(searching);

      searching = false;
  }
}


// Thread::cutoff_occurred() checks whether a beta cutoff has occurred in the
// current active split point, or in some ancestor of the split point.

bool Thread::cutoff_occurred() const {

  for (SplitPoint* sp = activeSplitPoint; sp; sp = sp->parentSplitPoint)
      if (sp->cutoff)
          return true;

  return false;
}


// Thread::is_available_to() checks whether the thread is available to help the
// thread 'master' at a split point. An obvious requirement is that thread must
// be idle. With more than two threads, this is not sufficient: If the thread is
// the master of some split point, it is only available as a slave to the slaves
// which are busy searching the split point at the top of slaves split point
// stack (the "helpful master concept" in YBWC terminology).

bool Thread::is_available_to(const Thread* master) const {

  if (searching)
      return false;

  // Make a local copy to be sure doesn't become zero under our feet while
  // testing next condition and so leading to an out of bound access.
  int size = splitPointsSize;

  // No split points means that the thread is available as a slave for any
  // other thread otherwise apply the "helpful master" concept if possible.
  return !size || (splitPoints[size - 1].slavesMask & (1ULL << master->idx));
}


// init() is called at startup to create and launch requested threads, that will
// go immediately to sleep due to 'sleepWhileIdle' set to true. We cannot use
// a c'tor becuase Threads is a static object and we need a fully initialized
// engine at this point due to allocation of Endgames in Thread c'tor.

void ThreadPool::init() {

  sleepWhileIdle = true;
  timer = new_thread<TimerThread>();
  push_back(new_thread<MainThread>());
  read_uci_options();
}


// exit() cleanly terminates the threads before the program exits

void ThreadPool::exit() {

  delete_thread(timer); // As first because check_time() accesses threads data

  for (Thread* th : *this)
      delete_thread(th);
}


// read_uci_options() updates internal threads parameters from the corresponding
// UCI options and creates/destroys threads to match the requested number. Thread
// objects are dynamically allocated to avoid creating in advance all possible
// threads, with included pawns and material tables, if only few are used.

void ThreadPool::read_uci_options() {

  maxThreadsPerSplitPoint = Options["Max Threads per Split Point"];
  minimumSplitDepth       = Options["Min Split Depth"] * ONE_PLY;
  size_t requested        = Options["Threads"];

  assert(requested > 0);

  // Value 0 has a special meaning: We determine the optimal minimum split depth
  // automatically. Anyhow the minimumSplitDepth should never be under 4 plies.
  if (!minimumSplitDepth)
      minimumSplitDepth = (requested < 8 ? 4 : 7) * ONE_PLY;
  else
      minimumSplitDepth = std::max(4 * ONE_PLY, minimumSplitDepth);

  while (size() < requested)
      push_back(new_thread<Thread>());

  while (size() > requested)
  {
      delete_thread(back());
      pop_back();
  }
}


// slave_available() tries to find an idle thread which is available as a slave
// for the thread 'master'.

Thread* ThreadPool::available_slave(const Thread* master) const {

  for (Thread* th : *this)
      if (th->is_available_to(master))
          return th;

  return nullptr;
}


// split() does the actual work of distributing the work at a node between
// several available threads. If it does not succeed in splitting the node
// (because no idle threads are available), the function immediately returns.
// If splitting is possible, a SplitPoint object is initialized with all the
// data that must be copied to the helper threads and then helper threads are
// told that they have been assigned work. This will cause them to instantly
// leave their idle loops and call search(). When all threads have returned from
// search() then split() returns.

template <bool Fake>
void Thread::split(Position& pos, const Stack* ss, Value alpha, Value beta, Value* bestValue,
                   Move* bestMove, Depth depth, Move threatMove, int moveCount,
                   MovePicker* movePicker, int nodeType, bool cutNode) {

  assert(pos.pos_is_ok());
  assert(*bestValue <= alpha && alpha < beta && beta <= VALUE_INFINITE);
  assert(*bestValue > -VALUE_INFINITE);
  assert(depth >= Threads.minimumSplitDepth);
  assert(searching);
  assert(splitPointsSize < MAX_SPLITPOINTS_PER_THREAD);

  // Pick the next available split point from the split point stack
  SplitPoint& sp = splitPoints[splitPointsSize];

  sp.masterThread = this;
  sp.parentSplitPoint = activeSplitPoint;
  sp.slavesMask = 1ULL << idx;
  sp.depth = depth;
  sp.bestValue = *bestValue;
  sp.bestMove = *bestMove;
  sp.threatMove = threatMove;
  sp.alpha = alpha;
  sp.beta = beta;
  sp.nodeType = nodeType;
  sp.cutNode = cutNode;
  sp.movePicker = movePicker;
  sp.moveCount = moveCount;
  sp.pos = &pos;
  sp.nodes = 0;
  sp.cutoff = false;
  sp.ss = ss;

  // Try to allocate available threads and ask them to start searching setting
  // 'searching' flag. This must be done under lock protection to avoid concurrent
  // allocation of the same slave by another master.
  Threads.mutex.lock();
  sp.mutex.lock();

  splitPointsSize++;
  activeSplitPoint = &sp;
  activePosition = nullptr;

  size_t slavesCnt = 1; // This thread is always included
  Thread* slave;

  while (    (slave = Threads.available_slave(this)) != nullptr
         && ++slavesCnt <= Threads.maxThreadsPerSplitPoint && !Fake)
  {
      sp.slavesMask |= 1ULL << slave->idx;
      slave->activeSplitPoint = &sp;
      slave->searching = true; // Slave leaves idle_loop()
      slave->notify_one(); // Could be sleeping
  }

  // Everything is set up. The master thread enters the idle loop, from which
  // it will instantly launch a search, because its 'searching' flag is set.
  // The thread will return from the idle loop when all slaves have finished
  // their work at this split point.
  if (slavesCnt > 1 || Fake)
  {
      sp.mutex.unlock();
      Threads.mutex.unlock();

      Thread::idle_loop(); // Force a call to base class idle_loop()

      // In helpful master concept a master can help only a sub-tree of its split
      // point, and because here is all finished is not possible master is booked.
      assert(!searching);
      assert(!activePosition);

      // We have returned from the idle loop, which means that all threads are
      // finished. Note that setting 'searching' and decreasing splitPointsSize is
      // done under lock protection to avoid a race with Thread::is_available_to().
      Threads.mutex.lock();
      sp.mutex.lock();
  }

  searching = true;
  splitPointsSize--;
  activeSplitPoint = sp.parentSplitPoint;
  activePosition = &pos;
  pos.set_nodes_searched(pos.nodes_searched() + sp.nodes);
  *bestMove = sp.bestMove;
  *bestValue = sp.bestValue;

  sp.mutex.unlock();
  Threads.mutex.unlock();
}

// Explicit template instantiations
template void Thread::split<false>(Position&, const Stack*, Value, Value, Value*, Move*, Depth, Move, int, MovePicker*, int, bool);
template void Thread::split< true>(Position&, const Stack*, Value, Value, Value*, Move*, Depth, Move, int, MovePicker*, int, bool);


// wait_for_think_finished() waits for main thread to go to sleep then returns

void ThreadPool::wait_for_think_finished() {

  std::unique_lock<std::mutex> lk(main()->mutex);
  sleepCondition.wait(lk, [&]{ return !main()->thinking; });
}


// start_thinking() wakes up the main thread sleeping in MainThread::idle_loop()
// so to start a new search, then returns immediately.

void ThreadPool::start_thinking(const Position& pos, const LimitsType& limits,
                                const std::vector<Move>& searchMoves, StateStackPtr& states) {
  wait_for_think_finished();

  SearchTime = Time::now(); // As early as possible

  Signals.stopOnPonderhit = Signals.firstRootMove = false;
  Signals.stop = Signals.failedLowAtRoot = false;

  RootMoves.clear();
  RootPos = pos;
  Limits = limits;
  if (states.get()) // If we don't set a new position, preserve current state
  {
      SetupStates = std::move(states); // Ownership transfer here
      assert(!states.get());
  }

  for (const ExtMove& ms : MoveList<LEGAL>(pos))
      if (   searchMoves.empty()
          || std::count(searchMoves.begin(), searchMoves.end(), ms.move))
          RootMoves.push_back(RootMove(ms.move));

  main()->thinking = true;
  main()->notify_one(); // Starts main thread
}
