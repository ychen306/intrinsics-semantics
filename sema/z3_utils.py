import z3
import operator
import functools

z3op_names = {
    z3.Z3_OP_AND: 'and',
    z3.Z3_OP_OR: 'or',
    z3.Z3_OP_XOR: 'xor',
    z3.Z3_OP_FALSE: 'false',
    z3.Z3_OP_TRUE: 'true',
    z3.Z3_OP_NOT: 'not',
    z3.Z3_OP_ITE: 'ite',
    z3.Z3_OP_BAND : 'bvand',
    z3.Z3_OP_BOR : 'bvor',
    z3.Z3_OP_BXOR : 'bvxor',
    z3.Z3_OP_BNOT : 'bvnot',
    z3.Z3_OP_BNEG : 'bvneg',
    z3.Z3_OP_CONCAT : 'concat',
    z3.Z3_OP_ULT : 'bvult',
    z3.Z3_OP_ULEQ : 'bvule',
    z3.Z3_OP_SLT : 'bvslt',
    z3.Z3_OP_SLEQ : 'bvsle',
    z3.Z3_OP_UGT : 'bvugt',
    z3.Z3_OP_UGEQ : 'bvuge',
    z3.Z3_OP_SGT : 'bvsgt',
    z3.Z3_OP_SGEQ : 'bvsge',
    z3.Z3_OP_BADD : 'bvadd',
    z3.Z3_OP_BMUL : 'bvmul',
    z3.Z3_OP_BUDIV : 'bvudiv',
    z3.Z3_OP_BSDIV : 'bvsdiv',
    z3.Z3_OP_BUREM : 'bvurem', 
    z3.Z3_OP_BSREM : 'bvsrem',
    z3.Z3_OP_BSMOD : 'bvsmod', 
    z3.Z3_OP_BSHL : 'bvshl',
    z3.Z3_OP_BLSHR : 'bvlshr',
    z3.Z3_OP_BASHR : 'bvashr',
    z3.Z3_OP_BSUB : 'bvsub',
    z3.Z3_OP_EQ : '=',
    z3.Z3_OP_DISTINCT : 'distinct',

    z3.Z3_OP_BSDIV_I:  'bvsdiv',
    z3.Z3_OP_BUDIV_I:  'bvudiv',
    z3.Z3_OP_BSREM_I:  'bvsrem',
    z3.Z3_OP_BUREM_I:  'bvurem',
    z3.Z3_OP_BSMOD_I:  'bvsmod',

    ## z3.Z3_OP_SIGN_EXT: lambda args, expr: self.mgr.BVSExt(args[0], z3.get_payload(expr, 0)),
    ## z3.Z3_OP_ZERO_EXT: lambda args, expr: self.mgr.BVZExt(args[0], z3.get_payload(expr, 0)),
    ## z3.Z3_OP_EXTRACT: lambda args, expr: self.mgr.BVExtract(args[0],
    }

def get_vars(f):
    r = set()
    def collect(f):
      if z3.is_const(f):
          if f.decl().kind() == z3.Z3_OP_UNINTERPRETED and not askey(f) in r:
              r.add(askey(f))
      else:
          for c in f.children():
              collect(c)
    collect(f)
    return r

def assoc_op(op):
  return lambda *xs: functools.reduce(op, xs)

class AstRefKey:
    def __init__(self, n):
        self.n = n
    def __hash__(self):
        return self.n.hash()
    def __eq__(self, other):
        return self.n.eq(other.n)
    def __repr__(self):
        return str(self.n)

def askey(n):
    assert isinstance(n, z3.AstRef)
    return AstRefKey(n)

def get_z3_app(e):
  decl = z3.Z3_get_app_decl(z3.main_ctx().ref(), e.ast)
  return z3.Z3_get_decl_kind(z3.main_ctx().ref(), decl)

def eval_z3_expr(e, args):
  return z3.simplify(z3.substitute(e, *args))

s = z3.Solver()
def equivalent(a, b, test_cases):
  if a.sort() != b.sort():
    return False

  return all(z3.is_true(eval_z3_expr(a==b, test_case))
      for test_case in test_cases)

def serialize_z3_expr(expr):
  s.push()
  s.add(expr == 0)
  dummy_bench = s.to_smt2()
  s.pop()
  return dummy_bench

def deserialize_z3_expr(serialized):
  s = z3.Solver()
  s.from_string(serialized)
  return s.assertions()[0].children()[0]

def get_used_bit_range(f, x):
  '''
  get the range of bits of x used in f
  '''
  bit_range = None
  def update_bit_range(new_range):
    nonlocal bit_range
    if bit_range is None:
      bit_range = new_range
    lo = min(bit_range[0], new_range[0])
    hi = max(bit_range[1], new_range[1])
    bit_range = lo, hi

  visited = set()
  def visit(f):
    if f in visited:
      return
    visited.add(f)
    if z3.is_app_of(f, z3.Z3_OP_EXTRACT):
      [base] = f.children()
      if base.get_id() == x.get_id():
        hi, lo = f.params()
        update_bit_range((lo, hi))
    for arg in f.children():
      visit(arg)
  visit(f)

  if bit_range is not None:
    lo, hi = bit_range
    # the parameters of z3.Extract is inclusive
    # make it exclusive
    return lo, hi+1

uninterpreted_funcs = {}
def get_uninterpreted_func(func_name, param_types):
  if func_name in uninterpreted_funcs:
    return uninterpreted_funcs[func_name]

  func = z3.Function(func_name, *param_types)
  uninterpreted_funcs[func_name] = func
  return func

def get_signed_max(bitwidth):
  return (1<<(bitwidth-1))-1

def get_signed_min(bitwidth):
  return -get_signed_max(bitwidth)-1

def get_unsigned_max(bitwidth):
  return (1<<bitwidth)-1

def get_unsigned_min(bitwidth):
  return 0

def saturate(x, in_bw, in_signed, out_bw, out_signed):
  hi = get_signed_max(out_bw) if out_signed else get_unsigned_max(out_bw)
  lo = get_signed_min(out_bw) if out_signed else get_unsigned_min(out_bw)
  lt = operator.lt if in_signed else z3.ULT
  gt = operator.gt if in_signed else z3.UGT
  return z3.If(
      gt(x, hi),
      hi,
      z3.If(
        lt(x, lo+1),
        lo,
        x))

def fix_bitwidth(x, bitwidth, signed=False):
  if x.size() < bitwidth:
    if signed:
      return z3.SignExt(bitwidth-x.size(), x)
    return z3.ZeroExt(bitwidth-x.size(), x)
  return z3.Extract(bitwidth-1, 0, x)

def get_saturator(in_size, out_size, signed):
  in_ty_str = ('s%d' if signed else 'u%d') % in_size
  out_ty_str = ('s%d' if signed else 'u%d') % out_size 
  in_ty_z3 = z3.BitVecSort(in_size)
  out_ty_z3 = z3.BitVecSort(out_size)
  builtin_name = 'Saturate_%s_to_%s' % (in_ty_str, out_ty_str)
  return get_uninterpreted_func(builtin_name, [in_ty_z3, out_ty_z3])

def concat(xs):
  if len(xs) == 1:
    return xs[0]
  return z3.Concat(*xs)

