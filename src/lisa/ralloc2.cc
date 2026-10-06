// Register allocation for the LISA port: the accumulator A as the one
// register, assigned to one-byte temporaries by the tree-decomposition
// based allocator in SDCCralloc.hpp.
//
// Based on the Padauk allocator,
// Philipp Klaus Krause, philipp@informatik.uni-frankfurt.de, pkk@spth.de, 2010 - 2018
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation; either version 2, or (at your option) any
// later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

// #define DEBUG_RALLOC_DEC // Uncomment to get debug messages while doing register allocation on the tree decomposition.
// #define DEBUG_RALLOC_DEC_ASS // Uncomment to get debug messages about assignments while doing register allocation on the tree decomposition (much more verbose than the one above).

#include "SDCCralloc.hpp"

extern "C"
{
  #include "ralloc.h"
  #include "gen.h"
  float dryLisaiCode (iCode *ic);
  void lisaDryRunInit (iCode *ic);
  bool lisa_assignment_optimal;
}

#define REG_A 0

// The code generator takes its operands from memory and uses A as
// scratch.  Any one-byte temporary can live in A: where a generator does
// not handle an A-resident operand natively, it parks it on the stack
// around the instruction (and saves a value that lives in A across an
// unrelated instruction the same way), at the cost of a push / pop that
// the dry run measures.  Only the instructions that branch cannot do
// that, since the pop would not be executed on the taken path; Ainst_ok
// below rules those placements out.

template <class I_t>
static void add_operand_conflicts_in_node(const cfg_node &n, I_t &I)
{
  // Operands of one byte only go to a register: no byte conflicts within an operand.
}

// Return true, iff the operand is placed (partially) in r.
template <class G_t>
static bool operand_in_reg(const operand *o, reg_t r, const i_assignment_t &ia, unsigned short int i, const G_t &G)
{
  if(!o || !IS_SYMOP(o))
    return(false);

  if(r >= port->num_regs)
    return(false);

  operand_map_t::const_iterator oi, oi_end;
  for(boost::tie(oi, oi_end) = G[i].operands.equal_range(OP_SYMBOL_CONST(o)->key); oi != oi_end; ++oi)
    if(oi->second == ia.registers[r][1] || oi->second == ia.registers[r][0])
      return(true);

  return(false);
}

// The compare / bit test branches directly, as part of this instruction.
static bool fused_with_ifx(const iCode *ic)
{
  if(ic->op != '<' && ic->op != '>' && ic->op != EQ_OP && ic->op != NE_OP && ic->op != BITWISEAND)
    return(false);
  return(IS_ITEMP(IC_RESULT(ic)) && OP_SYMBOL_CONST(IC_RESULT(ic))->regType == REG_CND);
}

template <class G_t, class I_t>
static bool Ainst_ok(const assignment &a, unsigned short int i, const G_t &G, const I_t &I)
{
  const iCode *ic = G[i].ic;
  const i_assignment_t &ia = a.i_assignment;

  if(ia.registers[REG_A][1] < 0)
    return(true);       // Register a not in use.

  if(ic->op == GOTO || ic->op == LABEL || ic->op == CRITICAL || ic->op == ENDCRITICAL)
    return(true);

  const cfg_dying_t &dying = G[i].dying;

  if(ic->op == IFX)
    {
      // The condition in A: bz / bnz keep A.  Otherwise A has to be free, since the test loads it.
      if(operand_in_reg(IC_COND(ic), REG_A, ia, i, G))
        return(true);
      return(dying.find(ia.registers[REG_A][1]) != dying.end() || dying.find(ia.registers[REG_A][0]) != dying.end());
    }

  if(ic->op == JUMPTABLE)
    {
      if(operand_in_reg(IC_JTCOND(ic), REG_A, ia, i, G))
        return(true);
      return(dying.find(ia.registers[REG_A][1]) != dying.end() || dying.find(ia.registers[REG_A][0]) != dying.end());
    }

  if(ic->op == RETURN)
    return(true);       // Nothing survives a return on its path.

  if(ic->op == IPUSH)
    return(true);       // Pushes with A kept (swap).

  const operand *left = IC_LEFT(ic);
  const operand *right = IC_RIGHT(ic);
  const operand *result = IC_RESULT(ic);

  bool result_in_A = operand_in_reg(result, REG_A, ia, i, G);
  bool left_in_A = operand_in_reg(left, REG_A, ia, i, G);
  bool right_in_A = operand_in_reg(right, REG_A, ia, i, G);

  // A can be clobbered: it holds the result, or whatever is in it dies here.
  bool dying_A = result_in_A || dying.find(ia.registers[REG_A][1]) != dying.end() || dying.find(ia.registers[REG_A][0]) != dying.end();

  if(ic->op == CALL || ic->op == PCALL || ic->op == IPUSH_VALUE_AT_ADDRESS || ic->op == INLINEASM)
    return(dying_A);    // The callee / the pushes / the user's code clobber A; the arguments are below it, so it cannot be saved.

  if(fused_with_ifx(ic))
    {
      if(left_in_A && right_in_A)
        return(false);
      if(!left_in_A && !right_in_A)
        return(dying_A);
      const operand *other = left_in_A ? right : left;
      if(getSize(operandType(left)) != 1 || getSize(operandType(right)) != 1)
        return(false);
      if(ic->op == EQ_OP || ic->op == NE_OP)
        return(true);   // cmp / cpi keep A.
      if(ic->op == BITWISEAND)
        return(dying_A);
      // Ordered: unsigned against a literal is a cpi, which keeps A; the other sequences clobber it,
      // and a signed compare against memory (it branches on the zero case) is not supported with an operand in A.
      sym_link *ltype = operandType(left), *rtype = operandType(right);
      bool usign = IS_PTR(ltype) || !IS_SPEC(ltype) || SPEC_USIGN(ltype) || IS_PTR(rtype) || !IS_SPEC(rtype) || SPEC_USIGN(rtype);
      if(usign)
        return(IS_OP_LITERAL(other) || dying_A);
      return(IS_OP_LITERAL(other) && dying_A);
    }

  return(true);         // The code generator parks operands on the stack where it has to.
}

template <class G_t, class I_t>
static void set_surviving_regs(const assignment &a, unsigned short int i, const G_t &G, const I_t &I)
{
  iCode *ic = G[i].ic;

  bitVectClear(ic->rMask);
  bitVectClear(ic->rSurv);

  cfg_alive_t::const_iterator v, v_end;
  for (v = G[i].alive.begin(), v_end = G[i].alive.end(); v != v_end; ++v)
    {
      if(a.global[*v] < 0)
        continue;
      ic->rMask = bitVectSetBit(ic->rMask, a.global[*v]);

      if(!(IC_RESULT(ic) && IS_SYMOP(IC_RESULT(ic)) && OP_SYMBOL_CONST(IC_RESULT(ic))->key == I[*v].v))
        if(G[i].dying.find(*v) == G[i].dying.end())
          ic->rSurv = bitVectSetBit(ic->rSurv, a.global[*v]);
    }
}

template <class G_t, class I_t>
static void assign_operand_for_cost(operand *o, const assignment &a, unsigned short int i, const G_t &G, const I_t &I)
{
  if(!o || !IS_SYMOP(o))
    return;
  symbol *sym = OP_SYMBOL(o);
  operand_map_t::const_iterator oi, oi_end;
  for(boost::tie(oi, oi_end) = G[i].operands.equal_range(OP_SYMBOL_CONST(o)->key); oi != oi_end; ++oi)
    {
      var_t v = oi->second;
      if(a.global[v] >= 0)
        {
          sym->regs[I[v].byte] = lisa_regs + a.global[v];
          sym->nRegs = I[v].size;
        }
      else
        {
          sym->regs[I[v].byte] = 0;
          sym->nRegs = I[v].size;
        }
    }
}

template <class G_t, class I_t>
static void assign_operands_for_cost(const assignment &a, unsigned short int i, const G_t &G, const I_t &I)
{
  const iCode *ic = G[i].ic;

  if(ic->op == IFX)
    assign_operand_for_cost(IC_COND(ic), a, i, G, I);
  else if(ic->op == JUMPTABLE)
    assign_operand_for_cost(IC_JTCOND(ic), a, i, G, I);
  else
    {
      assign_operand_for_cost(IC_LEFT(ic), a, i, G, I);
      assign_operand_for_cost(IC_RIGHT(ic), a, i, G, I);
      assign_operand_for_cost(IC_RESULT(ic), a, i, G, I);
    }
}

// Check that the operand is either fully in registers or fully in memory. Todo: Relax this once code generation can handle partially spilt variables!
template <class G_t, class I_t>
static bool operand_sane(const operand *o, const assignment &a, unsigned short int i, const G_t &G, const I_t &I)
{
  if(!o || !IS_SYMOP(o))
    return(true);

  operand_map_t::const_iterator oi, oi_end;
  boost::tie(oi, oi_end) = G[i].operands.equal_range(OP_SYMBOL_CONST(o)->key);

  if(oi == oi_end)
    return(true);

  // In registers.
  if(std::binary_search(a.local.begin(), a.local.end(), oi->second))
    {
      while(++oi != oi_end)
        if(!std::binary_search(a.local.begin(), a.local.end(), oi->second))
          return(false);
    }
  else
    {
       while(++oi != oi_end)
        if(std::binary_search(a.local.begin(), a.local.end(), oi->second))
          return(false);
    }

  return(true);
}

template <class G_t, class I_t>
static bool inst_sane(const assignment &a, unsigned short int i, const G_t &G, const I_t &I)
{
  const iCode *ic = G[i].ic;

  if(ic->op == IFX)
    return(operand_sane(IC_COND(ic), a, i, G, I));
  if(ic->op == JUMPTABLE)
    return(operand_sane(IC_JTCOND(ic), a, i, G, I));
  return(operand_sane(IC_RESULT(ic), a, i, G, I) && operand_sane(IC_LEFT(ic), a, i, G, I) && operand_sane(IC_RIGHT(ic), a, i, G, I));
}

// Cost function.
template <class G_t, class I_t>
static float instruction_cost(const assignment &a, unsigned short int i, const G_t &G, const I_t &I)
{
  iCode *ic = G[i].ic;
  float c;

  wassert(TARGET_IS_LISA);
  wassert(ic);

  if(!inst_sane(a, i, G, I))
    return(std::numeric_limits<float>::infinity());

  if(ic->generated)
    return(0.0f);

  if(!Ainst_ok(a, i, G, I))
    return(std::numeric_limits<float>::infinity());

  switch(ic->op)
    {
    // Register assignment doesn't matter for these:
    case FUNCTION:
    case ENDFUNCTION:
    case LABEL:
    case GOTO:
    case INLINEASM:
    case CRITICAL:
    case ENDCRITICAL:
      return(0.0f);
    case '!':
    case '~':
    case UNARYMINUS:
    case '+':
    case '-':
    case '^':
    case '|':
    case BITWISEAND:
    case IPUSH:
    case IPUSH_VALUE_AT_ADDRESS:
    case CALL:
    case PCALL:
    case RETURN:
    case '*':
    case '>':
    case '<':
    case EQ_OP:
    case NE_OP:
    case GETBYTE:
    case LEFT_OP:
    case RIGHT_OP:
    case GET_VALUE_AT_ADDRESS:
    case SET_VALUE_AT_ADDRESS:
    case '=':
    case IFX:
    case ADDRESS_OF:
    case JUMPTABLE:
    case CAST:
    case DUMMY_READ_VOLATILE:
      assign_operands_for_cost(a, i, G, I);
      set_surviving_regs(a, i, G, I);
      c = dryLisaiCode(ic);

      if (IC_RESULT (ic) && IS_ITEMP (IC_RESULT(ic)) && !OP_SYMBOL_CONST(IC_RESULT(ic))->remat && // Nudge towards saving RAM space.
        !operand_in_reg(IC_RESULT(ic), REG_A, a.i_assignment, i, G))
        c += 0.0001;

      ic->generated = false;
      return(c);
    default:
      return(0.0f);
    }
}

// For early removal of assignments that cannot be extended to valid assignments. This is just a dummy for now.
template <class G_t, class I_t>
static bool assignment_hopeless(const assignment &a, unsigned short int i, const G_t &G, const I_t &I, const var_t lastvar)
{
  return(false);
}

// Increase chance of finding good compatible assignments at join nodes.
template <class T_t>
static void get_best_local_assignment_biased(assignment &a, typename boost::graph_traits<T_t>::vertex_descriptor t, const T_t &T)
{
  a = *T[t].assignments.begin();

  varset_t newlocal;
  std::set_union(T[t].alive.begin(), T[t].alive.end(), a.local.begin(), a.local.end(), std::inserter(newlocal, newlocal.end()));
  a.local = newlocal;
}

// Suggest to honor register keyword.
template <class G_t, class I_t>
static float rough_cost_estimate(const assignment &a, unsigned short int i, const G_t &G, const I_t &I)
{
  const i_assignment_t &ia = a.i_assignment;
  float c = 0.0f;

  if(ia.registers[REG_A][1] < 0)
    c += 0.05f;

  varset_t::const_iterator v, v_end;
  for(v = a.local.begin(), v_end = a.local.end(); v != v_end; ++v)
    {
      const symbol *const sym = (symbol *)(hTabItemWithKey(liveRanges, I[*v].v));
      if(a.global[*v] < 0 && !sym->remat) // Try to put non-rematerializeable variables into registers.
        c += 0.1f;
      if(a.global[*v] < 0 && IS_REGISTER(sym->type)) // Try to honour register keyword.
        c += 4.0f;
    }

  return(c);
}

// Code for another ic is generated when generating this one. Mark the other as generated.
// The comparisons and the bit test branch directly when their result only feeds the following ifx.
static void extra_ic_generated(iCode *ic)
{
  if(ic->op != EQ_OP && ic->op != NE_OP && ic->op != '<' && ic->op != '>' && ic->op != BITWISEAND)
    return;

  iCode *ifx = ifxForOp(IC_RESULT(ic), ic);

  if(!ifx)
    return;

  OP_SYMBOL(IC_RESULT(ic))->for_newralloc = false;
  OP_SYMBOL(IC_RESULT(ic))->regType = REG_CND;
  ifx->generated = true;
}

template <class T_t, class G_t, class I_t>
static bool tree_dec_ralloc(T_t &T, G_t &G, const I_t &I)
{
  bool assignment_optimal;

  con2_t I2(boost::num_vertices(I));
  for(unsigned int i = 0; i < boost::num_vertices(I); i++)
    {
      I2[i].v = I[i].v;
      I2[i].byte = I[i].byte;
      I2[i].size = I[i].size;
      I2[i].name = I[i].name;
    }
  typename boost::graph_traits<I_t>::edge_iterator e, e_end;
  for(boost::tie(e, e_end) = boost::edges(I); e != e_end; ++e)
    add_edge(boost::source(*e, I), boost::target(*e, I), I2);

  assignment ac;
  assignment_optimal = true;
  tree_dec_ralloc_nodes(T, find_root(T), G, I2, ac, &assignment_optimal);

  const assignment &winner = *(T[find_root(T)].assignments.begin());

#ifdef DEBUG_RALLOC_DEC
  std::cout << "Winner: ";
  for(unsigned int i = 0; i < boost::num_vertices(I); i++)
    {
      std::cout << "(" << i << ", " << int(winner.global[i]) << ") ";
    }
  std::cout << "\n";
  std::cout << "Cost: " << winner.s << "\n";
  std::cout.flush();
#endif

  // Todo: Make this an assertion
  if(winner.global.size() != boost::num_vertices(I))
    {
      std::cerr << "ERROR: No Assignments at root\n";
      exit(-1);
    }

  for(unsigned int v = 0; v < boost::num_vertices(I); v++)
    {
      symbol *sym = (symbol *)(hTabItemWithKey(liveRanges, I[v].v));
      bool spilt = false;

      if(winner.global[v] >= 0)
        sym->regs[I[v].byte] = lisa_regs + winner.global[v];
      else
        {
          sym->regs[I[v].byte] = 0;
          spilt = true;
        }

      sym->nRegs = I[v].size;

      if(spilt)
        lisaSpillThis(sym);
    }

  for(unsigned int i = 0; i < boost::num_vertices(G); i++)
    set_surviving_regs(winner, i, G, I);

  return(!assignment_optimal);
}

iCode *lisa_ralloc2_cc(ebbIndex *ebbi)
{
  iCode *ic;

#ifdef DEBUG_RALLOC_DEC
  std::cout << "Processing " << currFunc->name << " from " << dstFileName << "\n"; std::cout.flush();
#endif

  cfg_t control_flow_graph;

  con_t conflict_graph;

  ic = create_cfg(control_flow_graph, conflict_graph, ebbi);

  if(optimize.genconstprop)
    recomputeValinfos(ic, ebbi, "_2");

  guessCounts(ic, ebbi);

  if(options.dump_graphs)
    dump_cfg(control_flow_graph);

  if(options.dump_graphs)
    dump_con(conflict_graph);

  tree_dec_t tree_decomposition;

  get_nice_tree_decomposition(tree_decomposition, control_flow_graph);

  alive_tree_dec(tree_decomposition, control_flow_graph);

  good_re_root(tree_decomposition);
  nicify(tree_decomposition);
  alive_tree_dec(tree_decomposition, control_flow_graph);

  if(options.dump_graphs)
    dump_tree_decomposition(tree_decomposition);

  guessCounts (ic, ebbi);

  // The dry runs need the frame layout of this function.
  lisaDryRunInit(ic);

  lisa_assignment_optimal = !tree_dec_ralloc(tree_decomposition, control_flow_graph, conflict_graph);

  return(ic);
}
