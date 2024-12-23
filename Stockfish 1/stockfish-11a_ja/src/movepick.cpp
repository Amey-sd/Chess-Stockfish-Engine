/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008 Marco Costalba

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


////
//// Includes
////

#include <cassert>

#include "history.h"
#include "evaluate.h"
#include "movegen.h"
#include "movepick.h"
#include "search.h"
#include "value.h"


////
//// Local definitions
////

namespace {

  /// Variables

  MovePicker::MovegenPhase PhaseTable[32];
  int MainSearchPhaseIndex;
  int EvasionsPhaseIndex;
  int QsearchWithChecksPhaseIndex;
  int QsearchNoCapturesPhaseIndex;
  int QsearchWithoutChecksPhaseIndex;
  int NoMovesPhaseIndex;

}



////
//// Functions
////


/// Constructor for the MovePicker class.  Apart from the position for which
/// it is asked to pick legal moves, MovePicker also wants some information
/// to help it to return the presumably good moves first, to decide which
/// moves to return (in the quiescence search, for instance, we only want to
/// search captures, promotions and some checks) and about how important good
/// move ordering is at the current node.

MovePicker::MovePicker(const Position& p, bool pv, Move ttm,
                       const SearchStack& ss, Depth d, EvalInfo* ei) : pos(p) {
  pvNode = pv;
  ttMove = ttm;
  mateKiller = (ss.mateKiller == ttm)? MOVE_NONE : ss.mateKiller;
  killer1 = ss.killers[0];
  killer2 = ss.killers[1];
  depth = d;
  movesPicked = 0;
  numOfMoves = 0;
  numOfBadCaptures = 0;

  // With EvalInfo we are able to know how many captures are possible before
  // generating them. So avoid generating in case we know are zero.
  Color us = pos.side_to_move();
  Color them = opposite_color(us);
  bool noCaptures =    ei
                   && (ei->attackedBy[us][0] & pos.pieces_of_color(them)) == 0
                   && !ei->mi->specialized_eval_exists()
                   && (pos.ep_square() == SQ_NONE)
                   && !pos.has_pawn_on_7th(us);

  if (p.is_check())
      phaseIndex = EvasionsPhaseIndex;
  else if (depth > Depth(0))
      phaseIndex = MainSearchPhaseIndex;
  else if (depth == Depth(0))
      phaseIndex = (noCaptures ? QsearchNoCapturesPhaseIndex : QsearchWithChecksPhaseIndex);
  else
      phaseIndex = (noCaptures ? NoMovesPhaseIndex : QsearchWithoutChecksPhaseIndex);

  dc = p.discovered_check_candidates(us);
  pinned = p.pinned_pieces(p.side_to_move());

  finished = false;
}


/// MovePicker::get_next_move() is the most important method of the MovePicker
/// class.  It returns a new legal move every time it is called, until there
/// are no more moves left of the types we are interested in.

Move MovePicker::get_next_move() {

  Move move;

  while (true)
  {
    // If we already have a list of generated moves, pick the best move from
    // the list, and return it.
    move = pick_move_from_list();
    if (move != MOVE_NONE)
    {
        assert(move_is_ok(move));
        return move;
    }

    // Next phase
    phaseIndex++;
    switch (PhaseTable[phaseIndex]) {

    case PH_TT_MOVE:
        if (ttMove != MOVE_NONE)
        {
            assert(move_is_ok(ttMove));
            if (move_is_legal(pos, ttMove, pinned))
                return ttMove;
        }
        break;

    case PH_MATE_KILLER:
        if (mateKiller != MOVE_NONE)
        {
            assert(move_is_ok(mateKiller));
            if (move_is_legal(pos, mateKiller, pinned))
                return mateKiller;
       }
       break;

    case PH_GOOD_CAPTURES:
        numOfMoves = generate_captures(pos, moves);
        score_captures();
        movesPicked = 0;
        break;

    case PH_BAD_CAPTURES:
        badCapturesPicked = 0;
        break;

    case PH_NONCAPTURES:
        numOfMoves = generate_noncaptures(pos, moves);
        score_noncaptures();
        movesPicked = 0;
        break;

    case PH_EVASIONS:
        assert(pos.is_check());
        numOfMoves = generate_evasions(pos, moves);
        score_evasions();
        movesPicked = 0;
        break;

    case PH_QCAPTURES:
        numOfMoves = generate_captures(pos, moves);
        score_qcaptures();
        movesPicked = 0;
        break;

    case PH_QCHECKS:
        numOfMoves = generate_checks(pos, moves, dc);
        movesPicked = 0;
        break;

    case PH_STOP:
        return MOVE_NONE;

    default:
        assert(false);
        return MOVE_NONE;
    }
  }
}


/// A variant of get_next_move() which takes a lock as a parameter, used to
/// prevent multiple threads from picking the same move at a split point.

Move MovePicker::get_next_move(Lock &lock) {

   lock_grab(&lock);
   if (finished)
   {
       lock_release(&lock);
       return MOVE_NONE;
   }
   Move m = get_next_move();
   if (m == MOVE_NONE)
       finished = true;

   lock_release(&lock);
   return m;
}


/// MovePicker::score_captures(), MovePicker::score_noncaptures(),
/// MovePicker::score_evasions() and MovePicker::score_qcaptures() assign a
/// numerical move ordering score to each move in a move list.  The moves
/// with highest scores will be picked first by pick_move_from_list().

void MovePicker::score_captures() {
  // Winning and equal captures in the main search are ordered by MVV/LVA.
  // Suprisingly, this appears to perform slightly better than SEE based
  // move ordering.  The reason is probably that in a position with a winning
  // capture, capturing a more valuable (but sufficiently defended) piece
  // first usually doesn't hurt.  The opponent will have to recapture, and
  // the hanging piece will still be hanging (except in the unusual cases
  // where it is possible to recapture with the hanging piece). Exchanging
  // big pieces before capturing a hanging piece probably helps to reduce
  // the subtree size.
  // While scoring captures it moves all captures with negative SEE values
  // to the badCaptures[] array.
  Move m;
  int seeValue;

  for (int i = 0; i < numOfMoves; i++)
  {
      m = moves[i].move;
      seeValue = pos.see(m);
      if (seeValue >= 0)
      {
          if (move_promotion(m))
              moves[i].score = QueenValueMidgame;
          else
              moves[i].score = int(pos.midgame_value_of_piece_on(move_to(m)))
                              -int(pos.type_of_piece_on(move_from(m)));
      }
      else
      {
          // Losing capture, move it to the badCaptures[] array
          assert(numOfBadCaptures < 63);
          moves[i].score = seeValue;
          badCaptures[numOfBadCaptures++] = moves[i];
          moves[i--] = moves[--numOfMoves];
      }
  }
}

void MovePicker::score_noncaptures() {
  // First score by history, when no history is available then use
  // piece/square tables values. This seems to be better then a
  // random choice when we don't have an history for any move.
  Move m;
  int hs;

  for (int i = 0; i < numOfMoves; i++)
  {
      m = moves[i].move;

      if (m == killer1)
          hs = HistoryMax + 2;
      else if (m == killer2)
          hs = HistoryMax + 1;
      else
          hs = H.move_ordering_score(pos.piece_on(move_from(m)), m);

      // Ensure history is always preferred to pst
      if (hs > 0)
          hs += 1000;

      // pst based scoring
      moves[i].score = hs + pos.mg_pst_delta(m);
  }
}

void MovePicker::score_evasions() {

  for (int i = 0; i < numOfMoves; i++)
  {
      Move m = moves[i].move;
      if (m == ttMove)
          moves[i].score = 2*HistoryMax;
      else if (!pos.square_is_empty(move_to(m)))
      {
          int seeScore = pos.see(m);
          moves[i].score = (seeScore >= 0)? seeScore + HistoryMax : seeScore;
      } else
          moves[i].score = H.move_ordering_score(pos.piece_on(move_from(m)), m);
  }
}

void MovePicker::score_qcaptures() {

  // Use MVV/LVA ordering
  for (int i = 0; i < numOfMoves; i++)
  {
      Move m = moves[i].move;
      if (move_promotion(m))
          moves[i].score = QueenValueMidgame;
      else
          moves[i].score = int(pos.midgame_value_of_piece_on(move_to(m)))
                          -int(pos.type_of_piece_on(move_from(m)));
  }
}


/// find_best_index() loops across the moves and returns index of
/// the highest scored one. There is also a second version that
/// lowers the priority of moves that attack the same square,
/// so that if the best move that attack a square fails the next
/// move picked attacks a different square if any, not the same one.

int MovePicker::find_best_index() {

  int bestScore = -10000000, bestIndex = -1;

  for (int i = movesPicked; i < numOfMoves; i++)
      if (moves[i].score > bestScore)
      {
          bestIndex = i;
          bestScore = moves[i].score;
      }
  return bestIndex;
}

int MovePicker::find_best_index(Bitboard* squares, int values[]) {

  int hs;
  Move m;
  Square to;
  int bestScore = -10000000, bestIndex = -1;

  for (int i = movesPicked; i < numOfMoves; i++)
  {
      m = moves[i].move;
      to = move_to(m);
      
      if (!bit_is_set(*squares, to))
      {
          // Init at first use
          set_bit(squares, to);
          values[to] = 0;
      }

      hs = moves[i].score - values[to];
      if (hs > bestScore)
      {
          bestIndex = i;
          bestScore = hs;
      }
  }

  if (bestIndex != -1)
  {
      // Raise value of the picked square, so next attack
      // to the same square will get low priority.
      to = move_to(moves[bestIndex].move);
      values[to] += 0xB00;
  }
  return bestIndex;
}


/// MovePicker::pick_move_from_list() picks the move with the biggest score
/// from a list of generated moves (moves[] or badCaptures[], depending on
/// the current move generation phase).  It takes care not to return the
/// transposition table move if that has already been serched previously.

Move MovePicker::pick_move_from_list() {

  int bestIndex;
  Move move;

  switch (PhaseTable[phaseIndex]) {
  case PH_GOOD_CAPTURES:
      assert(!pos.is_check());
      assert(movesPicked >= 0);

      while (movesPicked < numOfMoves)
      {
          bestIndex = find_best_index();

          if (bestIndex != -1) // Found a good capture
          {
              move = moves[bestIndex].move;
              moves[bestIndex] = moves[movesPicked++];
              if (   move != ttMove
                  && move != mateKiller
                  && pos.pl_move_is_legal(move, pinned))
                  return move;
          }
      }
      break;

  case PH_NONCAPTURES:
      assert(!pos.is_check());
      assert(movesPicked >= 0);

      while (movesPicked < numOfMoves)
      {
          // If this is a PV node or we have only picked a few moves, scan
          // the entire move list for the best move.  If many moves have already
          // been searched and it is not a PV node, we are probably failing low
          // anyway, so we just pick the first move from the list.
          bestIndex = (pvNode || movesPicked < 12) ? find_best_index() : movesPicked;

          if (bestIndex != -1)
          {
              move = moves[bestIndex].move;
              moves[bestIndex] = moves[movesPicked++];
              if (   move != ttMove
                  && move != mateKiller
                  && pos.pl_move_is_legal(move, pinned))
                  return move;
          }
      }
      break;

  case PH_EVASIONS:
      assert(pos.is_check());
      assert(movesPicked >= 0);

      while (movesPicked < numOfMoves)
      {
          bestIndex = find_best_index();

          if (bestIndex != -1)
          {
              move = moves[bestIndex].move;
              moves[bestIndex] = moves[movesPicked++];
              return move;
          }
    }
    break;

  case PH_BAD_CAPTURES:
      assert(!pos.is_check());
      assert(badCapturesPicked >= 0);
      // It's probably a good idea to use SEE move ordering here, instead
      // of just picking the first move.  FIXME
      while (badCapturesPicked < numOfBadCaptures)
      {
          move = badCaptures[badCapturesPicked++].move;
          if (   move != ttMove
              && move != mateKiller
              && pos.pl_move_is_legal(move, pinned))
              return move;
      }
      break;

  case PH_QCAPTURES:
      assert(!pos.is_check());
      assert(movesPicked >= 0);
      while (movesPicked < numOfMoves)
      {
          bestIndex = (movesPicked < 4 ? find_best_index() : movesPicked);

          if (bestIndex != -1)
          {
              move = moves[bestIndex].move;
              moves[bestIndex] = moves[movesPicked++];
              // Remember to change the line below if we decide to hash the qsearch!
              // Maybe also postpone the legality check until after futility pruning?
              if (/* move != ttMove && */ pos.pl_move_is_legal(move, pinned))
                  return move;
          }
      }
      break;

  case PH_QCHECKS:
      assert(!pos.is_check());
      assert(movesPicked >= 0);
      // Perhaps we should do something better than just picking the first
      // move here?  FIXME
      while (movesPicked < numOfMoves)
      {
          move = moves[movesPicked++].move;
          // Remember to change the line below if we decide to hash the qsearch!
          if (/* move != ttMove && */ pos.pl_move_is_legal(move, pinned))
              return move;
      }
      break;

  default:
      break;
  }
  return MOVE_NONE;
}


/// MovePicker::current_move_type() returns the type of the just
/// picked next move. It can be used in search to further differentiate
/// according to the current move type: capture, non capture, escape, etc.
MovePicker::MovegenPhase MovePicker::current_move_type() const {

  return PhaseTable[phaseIndex];
}


/// MovePicker::init_phase_table() initializes the PhaseTable[],
/// MainSearchPhaseIndex, EvasionPhaseIndex, QsearchWithChecksPhaseIndex
/// QsearchNoCapturesPhaseIndex, QsearchWithoutChecksPhaseIndex and
/// NoMovesPhaseIndex variables. It is only called once during program
/// startup, and never again while the program is running.

void MovePicker::init_phase_table() {

  int i = 0;

  // Main search
  MainSearchPhaseIndex = i - 1;
  PhaseTable[i++] = PH_TT_MOVE;
  PhaseTable[i++] = PH_MATE_KILLER;
  PhaseTable[i++] = PH_GOOD_CAPTURES;
  // PH_KILLER_1 and PH_KILLER_2 are not yet used.
  // PhaseTable[i++] = PH_KILLER_1;
  // PhaseTable[i++] = PH_KILLER_2;
  PhaseTable[i++] = PH_NONCAPTURES;
  PhaseTable[i++] = PH_BAD_CAPTURES;
  PhaseTable[i++] = PH_STOP;

  // Check evasions
  EvasionsPhaseIndex = i - 1;
  PhaseTable[i++] = PH_EVASIONS;
  PhaseTable[i++] = PH_STOP;

  // Quiescence search with checks
  QsearchWithChecksPhaseIndex = i - 1;
  PhaseTable[i++] = PH_QCAPTURES;
  PhaseTable[i++] = PH_QCHECKS;
  PhaseTable[i++] = PH_STOP;

  // Quiescence search with checks only and no captures
  QsearchNoCapturesPhaseIndex = i - 1;
  PhaseTable[i++] = PH_QCHECKS;
  PhaseTable[i++] = PH_STOP;

  // Quiescence search without checks
  QsearchWithoutChecksPhaseIndex = i - 1;
  PhaseTable[i++] = PH_QCAPTURES;
  PhaseTable[i++] = PH_STOP;

  // Do not generate any move
  NoMovesPhaseIndex = i - 1;
  PhaseTable[i++] = PH_STOP;
}
