// Selective FP16 for the Mali library (biblioteca_shaders_mali.mjs --fp16).
//
// The Mali-G52 runs mediump (FP16) arithmetic at twice the rate of FP32, and its compiler takes the SPIR-V
// RelaxedPrecision decoration as mediump. This pass decorates the float math of FRAGMENT shaders only, and
// keeps at full precision everything that ends up in:
//   - a texture coordinate or any other sampling operand (lod, bias, gradients, offsets, depth reference):
//     FP16 has 10 mantissa bits, so a coordinate over a 1280 or 1600 texture would snap to blocks;
//   - the depth output (FragDepth);
//   - a conversion to integer (indices, bit tricks).
// Float comparisons are NOT protected. The alpha test compares the final alpha (in [0, 1], where FP16 has a
// step of 1/2048), and the Xenos translation uses comparisons as arithmetic (sge/slt + select): protecting
// them dragged the whole color chain back to FP32 (1153 relaxed results against 21471 protected ids).
// That set is the backward slice of those operands over the SSA graph (loads follow the stores to the same
// pointer). Everything else that produces a 32-bit float (scalar or vector) gets RelaxedPrecision: color,
// lighting, fog, blending math and the results of the samples themselves.
//
// Operand words are taken as ids whenever they name a defined id. A literal that happens to look like an id
// only makes the slice bigger, never smaller, so the mistake is always on the safe side.

const OP = {
  ExtInst: 12, EntryPoint: 15, Capability: 17, TypeFloat: 22, TypeVector: 23, Variable: 59, Load: 61,
  Store: 62, AccessChain: 65, InBoundsAccessChain: 66, Decorate: 71, MemberDecorate: 72,
  VectorShuffle: 79, CompositeConstruct: 80, CompositeExtract: 81, CompositeInsert: 82,
  ConvertFToU: 109, ConvertFToS: 110, Bitcast: 124, FNegate: 127, FAdd: 129, FSub: 131, FMul: 133,
  FDiv: 136, FRem: 140, FMod: 141, VectorTimesScalar: 142, Dot: 148, Select: 169, Phi: 245,
};
const DEC_RELAXED = 0;
const DEC_BUILTIN = 11;
const BUILTIN_FRAG_DEPTH = 22;
const EXEC_FRAGMENT = 4;
// Image operations whose operands (besides the result type and id) must keep full precision.
const IMAGEN = new Set([87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106,
  107]);
// Results that may be relaxed when they are 32-bit floats outside the slice.
const RELAJABLES = new Set([OP.FNegate, OP.FAdd, OP.FSub, OP.FMul, OP.FDiv, OP.FRem, OP.FMod,
  OP.VectorTimesScalar, OP.Dot, OP.ExtInst, OP.Select, OP.Phi, OP.VectorShuffle, OP.CompositeConstruct,
  OP.CompositeExtract, OP.CompositeInsert, 87, 88, 91, 92, 96]);

export function relajarPrecision(entrada) {
  const w = entrada;
  const insts = [];
  for (let i = 5; i < w.length;) {
    const n = w[i] >>> 16;
    if (!n) throw new Error('SPIR-V corrupto');
    insts.push({ op: w[i] & 0xffff, at: i, n });
    i += n;
  }
  const palabra = (ins, k) => w[ins.at + k];
  let fragmento = false;
  const flotantes = new Set();  // ids of float32 and float32 vector types
  const defs = new Map();       // result id -> instruction
  const conTipo = new Map();    // result id -> result type id
  const decoradas = new Set();  // ids already RelaxedPrecision
  const profundidad = new Set();  // FragDepth variables
  const stores = new Map();     // pointer -> stored ids
  let primerTipo = -1;
  for (const ins of insts) {
    const { op } = ins;
    if (op === OP.EntryPoint && palabra(ins, 1) === EXEC_FRAGMENT) fragmento = true;
    if (op === OP.TypeFloat && palabra(ins, 2) === 32) flotantes.add(palabra(ins, 1));
    if (op === OP.TypeVector && flotantes.has(palabra(ins, 2))) flotantes.add(palabra(ins, 1));
    if (primerTipo < 0 && op >= 19 && op <= 39) primerTipo = ins.at;
    if (op === OP.Decorate && palabra(ins, 2) === DEC_RELAXED) decoradas.add(palabra(ins, 1));
    if (op === OP.Decorate && palabra(ins, 2) === DEC_BUILTIN && palabra(ins, 3) === BUILTIN_FRAG_DEPTH) {
      profundidad.add(palabra(ins, 1));
    }
    if (op === OP.Store) {
      const p = palabra(ins, 1);
      if (!stores.has(p)) stores.set(p, []);
      stores.get(p).push(palabra(ins, 2));
    }
  }
  if (!fragmento || primerTipo < 0) return { spirv: entrada, relajadas: 0, protegidas: 0 };
  // Result-producing instructions: [type, id, operands...] for every opcode we look at. Instructions without
  // a result type (stores, branches) are not definitions.
  for (const ins of insts) {
    if (ins.n >= 3 && ins.op !== OP.Decorate && ins.op !== OP.MemberDecorate && ins.op !== OP.Store &&
        ins.op !== OP.EntryPoint && !(ins.op >= 19 && ins.op <= 39)) {
      defs.set(palabra(ins, 2), ins);
      conTipo.set(palabra(ins, 2), palabra(ins, 1));
    }
  }
  const protegidos = new Set();
  const pendientes = [];
  const proteger = (id) => {
    if (!protegidos.has(id) && (defs.has(id) || stores.has(id))) {
      protegidos.add(id);
      pendientes.push(id);
    }
  };
  for (const ins of insts) {
    if (IMAGEN.has(ins.op)) {
      // Sampling operands: everything after the sampled image (coordinate, dref, image operands).
      for (let k = 4; k < ins.n; ++k) proteger(palabra(ins, k));
    } else if (ins.op === OP.ConvertFToU || ins.op === OP.ConvertFToS || ins.op === OP.Bitcast) {
      proteger(palabra(ins, 3));
    } else if (ins.op === OP.Store && profundidad.has(palabra(ins, 1))) {
      proteger(palabra(ins, 2));
    }
  }
  while (pendientes.length) {
    const id = pendientes.pop();
    for (const v of stores.get(id) || []) proteger(v);
    const ins = defs.get(id);
    if (!ins) continue;
    // ExtInst: word 3 is the instruction set and word 4 the instruction number (literals), operands from 5.
    const desde = ins.op === OP.ExtInst ? 5 : 3;
    for (let k = desde; k < ins.n; ++k) proteger(palabra(ins, k));
  }
  const nuevas = [];
  for (const ins of insts) {
    if (!RELAJABLES.has(ins.op) || ins.n < 3) continue;
    const id = palabra(ins, 2);
    if (flotantes.has(palabra(ins, 1)) && !protegidos.has(id) && !decoradas.has(id)) nuevas.push(id);
  }
  if (!nuevas.length) return { spirv: entrada, relajadas: 0, protegidas: protegidos.size };
  const salida = new Uint32Array(w.length + nuevas.length * 3);
  salida.set(w.subarray(0, primerTipo), 0);
  let o = primerTipo;
  for (const id of nuevas) {
    salida[o++] = (3 << 16) | OP.Decorate;
    salida[o++] = id;
    salida[o++] = DEC_RELAXED;
  }
  salida.set(w.subarray(primerTipo), o);
  return { spirv: salida, relajadas: nuevas.length, protegidas: protegidos.size };
}
