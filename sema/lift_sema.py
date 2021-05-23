'''
Lift smt formula to an IR similar to LLVM (minus control flow)
'''

from collections import namedtuple, defaultdict
import z3_utils
import z3
import bisect
import functools
import operator
import math
import fp_sema
from ir import *

def trunc(x, size):
  return Instruction(op='Trunc', bitwidth=size, args=[x])

bitwidth_table = [1, 8, 16, 32, 64]

reduction_ops = {
    'Add': operator.add,
    'Mul': operator.mul,
    'And': operator.and_,
    'Or' : operator.or_,
    'Xor': operator.xor,
    }


def get_size(x):
  try:
    return x.bitwidth
  except:
    return x.size()

def round_bitwidth(bw):
  idx = bisect.bisect_left(bitwidth_table, bw)
  assert idx < len(bitwidth_table), "bitwidth too large for scalar operation"
  return bitwidth_table[idx]

def trunc_zero(x):
  '''
  truncate all known zero bits from x
  '''
  args = x.children()

  # match a `concat 0, x1`
  if (z3.is_app_of(x, z3.Z3_OP_CONCAT) and
      len(args) == 2 and
      z3.is_bv_value(args[0]) and
      args[0].as_long() == 0):
    return args[1]

  # match const
  if z3.is_bv_value(x):
    size = len(bin(x.as_long()))-2
    return z3.BitVecVal(x.as_long(), size)

  return x

def is_var(x):
  return (z3.is_app_of(x, z3.Z3_OP_UNINTERPRETED) and
      len(x.children()) == 0)
class MatchFailure(Exception):
  pass

def match_with(f, op, num_args):
  if not z3.is_app_of(f, op):
    raise MatchFailure
  if len(f.children()) != num_args:
    raise MatchFailure
  return f.children()

def assert_const(f, const):
  if z3.is_bv_value(f) and f.as_long() == const:
    return
  raise MatchFailure

def assert_pred(pred):
  if not pred:
    raise MatchFailure

def elim_dead_branches(f):
  '''
  remove provably dead branches in z3.If
  '''
  s = z3.Solver()

  cache = {}
  def memoize(elim):
    def wrapped(f):
      key = z3_utils.askey(f)
      if key in cache:
        return cache[key]
      new_f = elim(f)
      cache[key] = new_f
      return new_f
    return wrapped

  @memoize
  def elim(f):
    if z3.is_app_of(f, z3.Z3_OP_ITE):
      cond, a, b = f.children()
      always_true = s.check(z3.Not(cond)) == z3.unsat
      if always_true:
        return elim(a)
      always_false = s.check(cond) == z3.unsat
      if always_false:
        return elim(b)

      cond2 = elim(cond)

      # can't statically determine which branch, follow both!
      # 1) follow the true branch
      s.push()
      s.add(cond)
      a2 = elim(a)
      s.pop()

      # 2) follow the false branch
      s.push()
      s.add(z3.Not(cond))
      b2 = elim(b)
      s.pop()

      return z3.simplify(z3.If(cond2, a2, b2))
    else:
      args = f.children()
      new_args = [elim(arg) for arg in args]
      return z3.simplify(z3.substitute(f, *zip(args, new_args)))

  return elim(f)

def elim_redundant_branches(f):
  '''
  remove redundant branches
  '''
  s = z3.Solver()

  cache = {}
  def memoize(elim):
    def wrapped(f):
      key = z3_utils.askey(f)
      if key in cache:
        return cache[key]
      new_f = elim(f)
      cache[key] = new_f
      return new_f
    return wrapped

  @memoize
  def elim(f):
    if z3.is_app_of(f, z3.Z3_OP_ITE):
      cond, a, b = [elim(x) for x in f.children()]

      # guess we can put a in both side
      s.push()
      s.add(z3.Not(cond))
      use_a = s.check(a != b) == z3.unsat
      s.pop()

      if use_a:
        return a

      # guess we can put b in both side
      s.push()
      s.add(cond)
      use_b = s.check(a != b) == z3.unsat
      s.pop()

      if use_b:
        return b

      # a and b are different...
      return z3.simplify(z3.If(cond, a, b))
    else:
      args = f.children()
      new_args = [elim(arg) for arg in args]
      return z3.simplify(z3.substitute(f, *zip(args, new_args)))

  return elim(f)


def reduce_bitwidth(f):
  '''
  for a formula that looks like `f = op concat(0, a), concat(0, b)`
  try to convert it to `f = concat(0, (op conat(0, a), concat(0, b))`
  so that the inner concat (e.g., zext) is smaller

  do this similarly for `f = op concat(0, a), const`, where const
  has unnecessarily large bitwidth
  '''
  # mapping f -> bitwidth-reduced f
  reduced = {}

  def memoize(reducer):
    def wrapped(f):
      key = z3_utils.askey(f)
      if key in reduced:
        return reduced[key]
      f_reduced = reducer(f)
      reduced[key] = f_reduced
      return f_reduced
    return wrapped

  @memoize
  def reduce_bitwidth_rec(f):
    key = z3_utils.askey(f)
    if key in reduced:
      return reduced[key]

    op = z3_utils.get_z3_app(f)
    # attempt to recursively reduce the bitwidth of sub computation
    new_args = [reduce_bitwidth_rec(arg) for arg in f.children()]

    if op not in alu_op_constructor:
      return z3.simplify(z3.substitute(f, *zip(f.children(), new_args)))

    is_unsigned = True
    pre_zext_args = [trunc_zero(x) for x in new_args]
    if op == z3.Z3_OP_BADD:
      required_bits = max(x.size() for x in pre_zext_args) + len(new_args) - 1
    elif op == z3.Z3_OP_BMUL:
      required_bits = sum(x.size() for x in pre_zext_args)
    elif op in (z3.Z3_OP_BUDIV, z3.Z3_OP_BUDIV_I):
      required_bits = sum(x.size() for x in pre_zext_args)
    elif op == z3.Z3_OP_BLSHR:
      required_bits = pre_zext_args[0].size()
    elif op == z3.Z3_OP_BSHL:
      required_bits = f.size()
    elif op in (z3.Z3_OP_BUREM, z3.Z3_OP_BUREM_I):
      required_bits = pre_zext_args[0].size()
    else:
      # FIXME: also handle signed operation
      # give up
      return z3.simplify(f.decl()(*new_args))

    if is_unsigned:
      required_bits = max(required_bits, max(x.size() for x in pre_zext_args))
      zext_args = [
          z3.ZeroExt(required_bits-x.size(), x)
          for x in pre_zext_args
          ]
      f_reduced = alu_op_constructor[op](*zext_args)
      if f_reduced.size() > f.size(): # give up
        return z3.simplify(alu_op_constructor[op](*new_args))
      return z3.simplify(z3.ZeroExt(f.size()-f_reduced.size(), f_reduced))

  return reduce_bitwidth_rec(f)

def typecheck(dag):
  '''
  * make sure the bitwidths match up
  * bitwidths are scalar bitwidth (e.g., 64)
  '''
  for value in dag.values():
    assert type(value) in ir_types
    if isinstance(value, Instruction):
      if value.op in binary_ops:
        args = [dag[arg] for arg in value.args]
        ok = (all(x.bitwidth == args[0].bitwidth for x in args) and
            args[0].bitwidth == value.bitwidth)
      elif value.op in cmp_ops:
        a, b = [dag[arg] for arg in value.args]
        ok = (a.bitwidth == b.bitwidth and value.bitwidth == 1)
      elif value.op == 'Select':
        k, a, b = [dag[arg] for arg in value.args]
        ok = (k.bitwidth == 1 and a.bitwidth == b.bitwidth == value.bitwidth)
      else:
        assert value.op in ['ZExt', 'SExt', 'Trunc']
        ok = True
      if not ok:
        return False
  return True

def reduction(op, ident):
  return lambda *xs: functools.reduce(op, xs, ident)

alu_op_constructor = {
    z3.Z3_OP_BADD : reduction(operator.add, ident=0),
    z3.Z3_OP_BMUL : reduction(operator.mul, ident=1),
    z3.Z3_OP_BUDIV : z3.UDiv,
    z3.Z3_OP_BUREM : z3.URem,
    z3.Z3_OP_BLSHR : z3.LShR,
    z3.Z3_OP_BSHL : operator.lshift,

    z3.Z3_OP_BSDIV : lambda a, b: a/b,

    z3.Z3_OP_BSMOD : operator.mod,
    z3.Z3_OP_BASHR : operator.rshift,
    z3.Z3_OP_BSUB : operator.sub,

    z3.Z3_OP_BSDIV_I: lambda a, b: a/b,
    z3.Z3_OP_BUDIV_I: z3.UDiv,

    z3.Z3_OP_BUREM_I: z3.URem,
    z3.Z3_OP_BSMOD_I: operator.mod,
    }

op_table = {
    z3.Z3_OP_AND: 'And',
    z3.Z3_OP_OR: 'Or',
    z3.Z3_OP_XOR: 'Xor',
    #z3.Z3_OP_FALSE
    #z3.Z3_OP_TRUE
    z3.Z3_OP_ITE: 'Select',
    z3.Z3_OP_BAND : 'And',
    z3.Z3_OP_BOR : 'Or',
    z3.Z3_OP_BXOR : 'Xor',
    z3.Z3_OP_SIGN_EXT: 'SExt',
    z3.Z3_OP_ZERO_EXT: 'ZExt',
    #z3.Z3_OP_BNOT
    #z3.Z3_OP_BNEG
    #z3.Z3_OP_CONCAT
    z3.Z3_OP_ULT : 'Ult',
    z3.Z3_OP_ULEQ : 'Ule',
    z3.Z3_OP_SLT : 'Slt',
    z3.Z3_OP_SLEQ : 'Sle',
    z3.Z3_OP_UGT : 'Ugt',
    z3.Z3_OP_UGEQ : 'Uge',
    z3.Z3_OP_SGT : 'Sgt',

    z3.Z3_OP_SGEQ : 'Sge',
    z3.Z3_OP_BADD : 'Add',
    z3.Z3_OP_BMUL : 'Mul',
    z3.Z3_OP_BUDIV : 'UDiv',
    z3.Z3_OP_BSDIV : 'SDiv',
    z3.Z3_OP_BUREM : 'URem',
    #z3.Z3_OP_BSREM
    z3.Z3_OP_BSMOD : 'SRem',
    z3.Z3_OP_BSHL : 'Shl',
    z3.Z3_OP_BLSHR : 'LShr',
    z3.Z3_OP_BASHR : 'AShr',
    z3.Z3_OP_BSUB : 'Sub',
    z3.Z3_OP_EQ : 'Eq',

    z3.Z3_OP_DISTINCT : 'Ne',

    z3.Z3_OP_BSDIV_I: 'SDiv',
    z3.Z3_OP_BUDIV_I: 'UDiv',
    #z3.Z3_OP_BSREM_I
    z3.Z3_OP_BUREM_I: 'URem',
    z3.Z3_OP_BSMOD_I: 'SRem',
    }

# translation table from uninterp. func to our ir (basically LLVM)
float_ops = {
    'neg': 'FNeg',
    'add': 'FAdd',
    'sub': 'FSub',
    'mul': 'FMul',
    'div': 'FDiv',
    'lt': 'Folt',
    'le': 'Fole',
    'gt': 'Fogt',
    'ge': 'Foge',
    'ne': 'Fone',
    }

def is_simple_extraction(ext):
  '''
  check if `ext` is an extract on a variable
  '''
  [x] = ext.children()
  return (
      z3.is_app_of(x, z3.Z3_OP_UNINTERPRETED) and
      len(x.children()) == 0)

def partition_slices(slices):
  partition = set()
  for s in slices:
    for s2 in partition:
      if s.overlaps(s2):
        partition.remove(s2)
        partition.add(s.union(s2))
        break
    else:
      partition.add(s)
  return partition

class ExtractionHistory:
  '''
  this class records all of the extraction
  done on a set of live-in bitvector,
  '''
  def __init__(self):
    # list of extracted slices
    self.extracted_slices = defaultdict(list)
    self.id_counter = 0

  def record(self, ext):
    assert is_simple_extraction(ext)
    [x] = ext.children()
    hi, lo = ext.params()
    s = Slice(x, lo, hi+1)
    self.extracted_slices[x].append(s)
    return s

  def translate_slices(self, translator):
    '''
    return a map <slice> -> <ir>
    '''
    translated = {}
    for slices in self.extracted_slices.values():
      partition = partition_slices(slices)
      for s in slices:
        for root_slice in partition:
          if s.overlaps(root_slice):
            lo = s.lo - root_slice.lo
            hi = s.hi - root_slice.lo
            assert root_slice.size() >= s.size()
            if s == root_slice:
              translated[s] = root_slice
            elif lo == 0:
              # truncation
              translated[s] = trunc(
                  translator.translate(root_slice.to_z3()),
                  round_bitwidth(s.size()))
            else: # lo > 0
              # shift right + truncation
              #shift = Instruction(
              #    op='LShr',
              #    bitwidth=root_slice.size(),
              #    args=[root_slice])
              #translated[s] = trunc(shift, s.size())
              shift = translator.translate(z3.LShR(root_slice.to_z3(), lo))
              translated[s] = trunc(shift, round_bitwidth(s.size()))
            break
    return translated

def recover_sub(f):
  '''
  z3 simplifies `a + b` to `a + 0b111..11 * b`,
  but we want to turn this into subtraction
  '''
  if not z3.is_app_of(f, z3.Z3_OP_BADD):
    return f
  args = f.children()
  if len(args) != 2:
    return f
  a, b = args
  if not z3.is_app_of(b, z3.Z3_OP_BMUL):
    if z3.is_app_of(a, z3.Z3_OP_BMUL):
      b, a = a, b
    else:
      return f
  b_args = b.children()
  if len(b_args) != 2:
    return f
  b1, b2 = b_args
  if z3.is_true(z3.simplify(b1 == z3.BitVecVal(-1, b1.size()))):
    return a - b2
  return f

class Translator:
  def __init__(self):
    self.solver = z3.Solver()
    self.extraction_history = ExtractionHistory()
    self.z3op_translators = {
        z3.Z3_OP_TRUE: self.translate_true,
        z3.Z3_OP_FALSE: self.translate_false,
        z3.Z3_OP_NOT: self.translate_bool_not,
        z3.Z3_OP_BNOT: self.translate_not,
        z3.Z3_OP_BNEG: self.translate_neg,
        z3.Z3_OP_EXTRACT: self.translate_extract,
        z3.Z3_OP_CONCAT: self.translate_concat,
        z3.Z3_OP_UNINTERPRETED: self.translate_uninterpreted,
        z3.Z3_OP_BNUM: self.translate_constant,
        }
    # mapping <formula> -> <ir node id>
    self.translated = {}
    # translated IR
    self.ir = {}
    self.id_counter = 0

  def new_id(self):
    new_id = self.id_counter
    self.id_counter += 1
    return new_id

  def translate_constant(self, c):
    return Constant(value=c.as_long(), bitwidth=round_bitwidth(c.size()))

  def translate_formula(self, f, elem_size):
    '''
    entry point
    '''
    if not z3.is_app_of(f, z3.Z3_OP_CONCAT):
      outs = [self.translate(f)]
    else:
      assert(f.size() % elem_size == 0)
      num_elems = f.size() // elem_size
      elems = []
      partial_elem = []
      partial_size = 0
      offset = 0
      x_offset = 0
      for x in reversed(f.children()):
        while offset < x_offset + x.size():
          begin = offset - x_offset
          end = min(begin + elem_size - partial_size, x.size())
          chunk_size = end - begin
          partial_size += chunk_size
          offset += chunk_size
          partial_elem.append(z3.Extract(end-1, begin, x))

          if partial_size == elem_size:
            if len(partial_elem) == 1:
              elems.append(z3.simplify(partial_elem[0]))
            else:
              elems.append(z3.simplify(z3_utils.concat(partial_elem[::-1])))
            partial_elem = []
            partial_size = 0

        x_offset += x.size()

      elems.reverse()
      assert z3.Solver().check(z3_utils.concat(elems) != f) == z3.unsat

      outs = [self.translate(x) for x in elems]

    # translate the slices
    slice2ir = self.extraction_history.translate_slices(self)
    for node_id, node in self.ir.items():
      if isinstance(node, Slice):
        self.ir[node_id] = slice2ir[node]

    assert all(y in self.ir for y in outs)
    return outs, self.ir

  def translate(self, f):
    # detect `~(a | b)` and turn them into `~a & ~b`
    try:
      [x] = match_with(f, z3.Z3_OP_BNOT, 1)
      [a, b] = match_with(x, z3.Z3_OP_BOR, 2)
      # don't simplify the whole expression
      f = z3.simplify(~a) & z3.simplify(~b)
    except MatchFailure:
      pass

    if f in self.translated:
      return self.translated[f]
    f = recover_sub(f)
    node_id = self.new_id()
    z3op = z3_utils.get_z3_app(f)

    if z3op in self.z3op_translators:
      # see if there's a specialized translator
      node = self.z3op_translators[z3op](f)
    else:
      op = op_table[z3op]
      assert z3.is_bv(f) or z3.is_bool(f)
      # expand flattened reduction operator
      if op in reduction_ops:
        f = functools.reduce(reduction_ops[op], f.children())
      bitwidth = f.size() if z3.is_bv(f) else 1
      node = Instruction(
          op=op, bitwidth=round_bitwidth(bitwidth),
          args=[self.translate(arg) for arg in f.children()])

    self.translated[f] = node_id
    self.ir[node_id] = node
    return node_id

  def translate_true(*_):
    return Constant(z3.BitVecVal(1, 1))

  def translate_false(*_):
    return Constant(z3.BitVecVal(0, 1))

  def translate_bool_not(self, f):
    [x] = f.children()
    return Instruction(
        op='Xor',
        bitwidth=1,
        args=[
          self.translate(z3.BitVecVal(1,1)),
          self.translate(x)])

  def translate_not(self, f):
    [x] = f.children()
    # not x == xor -1, x
    node_id = self.translate((-1) ^ x)
    return self.ir[node_id]

  def translate_neg(self, f):
    [x] = f.children()
    # not x == sub 0, x
    node_id = self.translate(0-x)
    return self.ir[node_id]

  def translate_extract(self, ext):
    if is_simple_extraction(ext):
      s = self.extraction_history.record(ext)
      return s

    [x] = ext.children()
    assert x.size() <= 64,\
        "extraction too complex to model in scalar code"

    _, lo = ext.params()
    if lo > 0:
      translated = self.translate(z3.LShR(x, lo))
    else:
      translated = self.translate(x)
    translated_size = get_size(self.ir[translated])
    bw = round_bitwidth(ext.size())
    assert translated_size >= bw
    if translated_size == bw:
      return self.ir[translated]
    return trunc(translated, bw)

  def try_translate_sext(self, concat):
    '''
    don't even bother trying to pattern match this...
    just prove it
    '''
    x = concat.children()[-1]
    sext = z3.SignExt(concat.size()-x.size(), x)
    is_sext = self.solver.check(concat != sext) == z3.unsat
    if is_sext:
      return Instruction(
          op='SExt',
          bitwidth=round_bitwidth(concat.size()),
          args=[self.translate(x)])
    return None

  def translate_concat(self, concat):
    '''
    try to convert concat of sign bit to sext
    '''
    sext = self.try_translate_sext(concat)
    if sext is not None:
      return sext

    args = concat.children()
    assert len(args) == 2, "only support using concat for zext"
    a, b = args
    assert z3.is_bv_value(a) and a.as_long() == 0,\
        "only support using concat for zero extension"

    b_translated = self.translate(b)
    # there's a chance that we already upgraded the bitwidth of b
    # during translation (e.g. b.size = 17 and we normalize to 32)
    concat_size = round_bitwidth(concat.size())
    if self.ir[b_translated].bitwidth == concat_size:
      return self.ir[b_translated]
    return Instruction(
        op='ZExt',
        bitwidth=concat_size, args=[b_translated])

  def translate_saturation(self, f):
    sat_name = f.decl().name()
    [x] = f.children()

    _, in_name, _, out_name = sat_name.split('_')

    in_signed = in_name.startswith('s')
    in_bw = int(in_name[1:])

    out_signed = out_name.startswith('s')
    out_bw = int(out_name[1:])

    saturated = z3_utils.saturate(x, in_bw, in_signed, out_bw, out_signed)
    node_id = self.translate(z3.Extract(f.size()-1, 0, saturated))
    return self.ir[node_id]

  def translate_abs(self, f):
    name = f.decl().name()
    [x] = f.children()

    _, typename = name.split('_')
    is_int = typename.startswith('i')
    bitwidth = f.size()

    if is_int:
      y = z3.If(x < 0, -x , x)
    else:
      flt, _ = fp_sema.binary_float_cmp('lt', use_uninterpreted=True)
      fneg, _ = fp_sema.binary_float_op('neg', use_uninterpreted=True)
      y = z3.If(flt(x, fp_sema.fp_literal(0.0, bitwidth)), fneg(x), x)

    node_id = self.translate(y)
    return self.ir[node_id]

  def translate_uninterpreted(self, f):
    args = f.children()
    if len(args) == 0:
      # live-in
      return self.extraction_history.record(z3.Extract(f.size()-1, 0, f))

    func = f.decl().name()

    if func.startswith('Saturate'):
      return self.translate_saturation(f)

    if func.startswith('Abs'):
      return self.translate_abs(f)

    assert func.startswith('fp_')
    _, op, _ = func.split('_')

    if op == 'literal':
      assert z3.is_bv_val(arg)
      # jesus
      literal_val = float(eval(str(z3.simplify(z3.fpBVToFP(arg)))))
      return FPConstant(literal_val, arg.size())

    assert z3.is_bool(f) or f.size() in [32, 64]
    bitwidth = 1 if z3.is_bool(f) else f.size()
    return Instruction(
        op=float_ops[op], bitwidth=bitwidth,
        args=[self.translate(arg) for arg in f.children()])
