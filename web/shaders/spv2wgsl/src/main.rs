//! GTA IV's shaders (rewritten SPIR-V) to WGSL with naga, lowering what WGSL
//! can't say first: switch cases that fall through into the next case (the
//! Xenos control-flow dispatch loops). Such a switch becomes
//!
//!     active = false; continued = false;
//!     loop {
//!       if (active || selector matches case 0) { active = true; body 0 }
//!       if (active || selector matches case 1) { active = true; body 1 }
//!       ...
//!       break;
//!     }
//!     if (continued) { continue; }
//!
//! where a case that doesn't fall through ends in `break` (leaving the wrapper
//! loop, as it left the switch) and a `continue` aimed at the enclosing loop
//! sets `continued` and breaks out first.
//!
//! Then the descriptor heaps, which WebGPU doesn't have: each heap access
//! whose index is read from the shared constants (`shared.member[k].c`, a
//! constant slot) becomes its own binding, and the bind groups are renumbered
//! to fit WebGPU's four:
//!   group 0: the uniform blocks (0 vertex constants, 1 pixel constants,
//!            2 shared constants), as in group 6 before;
//!   group 1: textures, binding kind * 32 + slot (kind 0 2D, 1 2D array,
//!            2 3D, 3 cube);
//!   group 2: samplers, binding = slot.
//! The bindings used are listed on stderr as `bind <group> <binding> <kind>`.
//! Usage: spv2wgsl <in.spv> <out.wgsl>
use std::collections::HashMap;

use naga::{
    AddressSpace, GlobalVariable, ImageClass, ImageDimension, ResourceBinding,
    Range, BinaryOperator, Block, Expression, Function, Handle, Literal, LocalVariable,
    Module, Scalar, Span, Statement, SwitchValue, Type, TypeInner, UnaryOperator,
};

fn is_terminator(s: &Statement) -> bool {
    matches!(s, Statement::Break | Statement::Continue | Statement::Return { .. } | Statement::Kill)
}

struct Lowerer<'a> {
    fun: &'a mut Function,
    bool_ty: Handle<Type>,
    lowered: usize,
}

impl Lowerer<'_> {
    fn expr(&mut self, e: Expression) -> Handle<Expression> {
        self.fun.expressions.append(e, Span::UNDEFINED)
    }
    fn local(&mut self, name: &str) -> Handle<Expression> {
        let var = self.fun.local_variables.append(
            LocalVariable { name: Some(name.into()), ty: self.bool_ty, init: None },
            Span::UNDEFINED,
        );
        self.expr(Expression::LocalVariable(var))
    }
    fn literal_true(&mut self) -> Handle<Expression> {
        self.expr(Expression::Literal(Literal::Bool(true)))
    }
    fn store(block: &mut Block, pointer: Handle<Expression>, value: Handle<Expression>) {
        block.push(Statement::Store { pointer, value }, Span::UNDEFINED);
    }
    // Appends expressions built by `build` and emits them as one range.
    fn emitted(
        &mut self,
        block: &mut Block,
        build: impl FnOnce(&mut Self) -> Handle<Expression>,
    ) -> Handle<Expression> {
        let first = self.fun.expressions.len();
        let result = build(self);
        let handles: Vec<_> = self.fun.expressions.iter().skip(first).map(|(h, _)| h).collect();
        if let (Some(&a), Some(&b)) = (handles.first(), handles.last()) {
            block.push(Statement::Emit(Range::new_from_bounds(a, b)), Span::UNDEFINED);
        }
        result
    }

    fn block(&mut self, block: &mut Block) {
        let old = std::mem::take(block);
        for (mut statement, span) in old.span_into_iter() {
            match statement {
                Statement::Block(ref mut b) => self.block(b),
                Statement::If { ref mut accept, ref mut reject, .. } => {
                    self.block(accept);
                    self.block(reject);
                }
                Statement::Loop { ref mut body, ref mut continuing, .. } => {
                    self.block(body);
                    self.block(continuing);
                }
                Statement::Switch { ref mut cases, .. } => {
                    for case in cases.iter_mut() {
                        self.block(&mut case.body);
                    }
                }
                _ => {}
            }
            let needs = matches!(&statement, Statement::Switch { cases, .. }
                if cases.iter().any(|c| c.fall_through && !c.body.is_empty()));
            if !needs {
                block.push(statement, span);
                continue;
            }
            let Statement::Switch { selector, cases } = statement else { unreachable!() };
            self.lower_switch(block, selector, cases);
            self.lowered += 1;
        }
    }

    // Replaces `continue`s that would leave the switch (not those inside
    // nested loops) with `continued = true; break;`. Returns whether any did.
    fn redirect_continue(block: &mut Block, continued: Handle<Expression>, t: Handle<Expression>) -> bool {
        let mut any = false;
        let old = std::mem::take(block);
        for (mut statement, span) in old.span_into_iter() {
            match statement {
                Statement::Continue => {
                    Self::store(block, continued, t);
                    block.push(Statement::Break, span);
                    any = true;
                    continue;
                }
                Statement::Block(ref mut b) => any |= Self::redirect_continue(b, continued, t),
                Statement::If { ref mut accept, ref mut reject, .. } => {
                    any |= Self::redirect_continue(accept, continued, t);
                    any |= Self::redirect_continue(reject, continued, t);
                }
                // `break` in a nested switch still leaves that switch, but
                // `continue` there leaves ours too.
                Statement::Switch { ref mut cases, .. } => {
                    for case in cases.iter_mut() {
                        any |= Self::redirect_continue(&mut case.body, continued, t);
                    }
                }
                _ => {}
            }
            block.push(statement, span);
        }
        any
    }

    fn lower_switch(&mut self, out: &mut Block, selector: Handle<Expression>, cases: Vec<naga::SwitchCase>) {
        let active = self.local("ft_active");
        let continued = self.local("ft_continued");
        let t = self.literal_true();
        let f = self.expr(Expression::Literal(Literal::Bool(false)));
        Self::store(out, active, f);
        Self::store(out, continued, f);
        let values: Vec<SwitchValue> =
            cases.iter().map(|c| c.value).filter(|v| *v != SwitchValue::Default).collect();
        let mut body = Block::new();
        let mut any_continue = false;
        for case in cases {
            let literal = |v: SwitchValue| match v {
                SwitchValue::I32(i) => Literal::I32(i),
                SwitchValue::U32(u) => Literal::U32(u),
                SwitchValue::Default => unreachable!(),
            };
            // Literals need no emit; the comparisons and loads do.
            let lits: Vec<_> = match case.value {
                SwitchValue::Default => values.iter().map(|v| self.expr(Expression::Literal(literal(*v)))).collect(),
                v => vec![self.expr(Expression::Literal(literal(v)))],
            };
            let condition = self.emitted(&mut body, |s| {
                let load = s.expr(Expression::Load { pointer: active });
                let matched = if case.value == SwitchValue::Default {
                    let mut any = None;
                    for &l in &lits {
                        let eq = s.expr(Expression::Binary { op: BinaryOperator::Equal, left: selector, right: l });
                        any = Some(match any {
                            None => eq,
                            Some(prev) => s.expr(Expression::Binary { op: BinaryOperator::LogicalOr, left: prev, right: eq }),
                        });
                    }
                    match any {
                        Some(a) => s.expr(Expression::Unary { op: UnaryOperator::LogicalNot, expr: a }),
                        None => s.expr(Expression::Literal(Literal::Bool(true))),
                    }
                } else {
                    s.expr(Expression::Binary { op: BinaryOperator::Equal, left: selector, right: lits[0] })
                };
                s.expr(Expression::Binary { op: BinaryOperator::LogicalOr, left: load, right: matched })
            });
            let mut accept = Block::new();
            Self::store(&mut accept, active, t);
            let mut case_body = case.body;
            any_continue |= Self::redirect_continue(&mut case_body, continued, t);
            let ends = case_body.last().is_some_and(is_terminator);
            accept.extend_block(case_body);
            if !case.fall_through && !ends {
                accept.push(Statement::Break, Span::UNDEFINED);
            }
            body.push(Statement::If { condition, accept, reject: Block::new() }, Span::UNDEFINED);
        }
        body.push(Statement::Break, Span::UNDEFINED);
        out.push(Statement::Loop { body, continuing: Block::new(), break_if: None }, Span::UNDEFINED);
        if any_continue {
            let load = self.emitted(out, |s| s.expr(Expression::Load { pointer: continued }));
            let mut accept = Block::new();
            accept.push(Statement::Continue, Span::UNDEFINED);
            out.push(Statement::If { condition: load, accept, reject: Block::new() }, Span::UNDEFINED);
        }
    }
}

fn lower(module: &mut Module) -> usize {
    let bool_ty = module.types.insert(
        Type { name: None, inner: TypeInner::Scalar(Scalar::BOOL) },
        Span::UNDEFINED,
    );
    let mut lowered = 0;
    let mut run = |fun: &mut Function| {
        let mut body = std::mem::take(&mut fun.body);
        let mut l = Lowerer { fun, bool_ty, lowered: 0 };
        l.block(&mut body);
        lowered += l.lowered;
        fun.body = body;
    };
    for (_, fun) in module.functions.iter_mut() {
        run(fun);
    }
    for ep in module.entry_points.iter_mut() {
        run(&mut ep.function);
    }
    lowered
}

// Where in the shared constants an expression points or what it loaded, in
// 32-bit words.
#[derive(Clone, Copy)]
enum Place {
    Block,
    Array(u32),
    Vector(u32),
    Word(u32),
}

fn constant_index(fun: &Function, module: &Module, e: Handle<Expression>) -> Option<u32> {
    match fun.expressions[e] {
        Expression::Literal(Literal::U32(v)) => Some(v),
        Expression::Literal(Literal::I32(v)) => u32::try_from(v).ok(),
        Expression::Constant(c) => match module.global_expressions[module.constants[c].init] {
            Expression::Literal(Literal::U32(v)) => Some(v),
            Expression::Literal(Literal::I32(v)) => u32::try_from(v).ok(),
            _ => None,
        },
        _ => None,
    }
}

fn place(fun: &Function, module: &Module, shared: Handle<GlobalVariable>, e: Handle<Expression>) -> Option<Place> {
    match fun.expressions[e] {
        Expression::GlobalVariable(g) if g == shared => Some(Place::Block),
        Expression::Load { pointer } => place(fun, module, shared, pointer),
        Expression::AccessIndex { base, index } => step(place(fun, module, shared, base)?, index),
        Expression::Access { base, index } => {
            step(place(fun, module, shared, base)?, constant_index(fun, module, index)?)
        }
        _ => None,
    }
}

fn step(p: Place, index: u32) -> Option<Place> {
    match p {
        Place::Block if index == 0 => Some(Place::Array(0)),
        Place::Array(o) => Some(Place::Vector(o + index * 4)),
        Place::Vector(o) => Some(Place::Word(o + index)),
        _ => None,
    }
}

// Takes `skip` out of the block's emit ranges: global variable expressions
// are never emitted.
fn unemit(block: &mut Block, skip: &std::collections::HashSet<Handle<Expression>>) {
    let old = std::mem::take(block);
    for (mut statement, span) in old.span_into_iter() {
        match statement {
            Statement::Emit(ref range) => {
                let mut run: Vec<Handle<Expression>> = Vec::new();
                let flush = |run: &mut Vec<Handle<Expression>>, block: &mut Block| {
                    if let (Some(&a), Some(&b)) = (run.first(), run.last()) {
                        block.push(Statement::Emit(Range::new_from_bounds(a, b)), span);
                    }
                    run.clear();
                };
                for h in range.clone() {
                    if skip.contains(&h) {
                        flush(&mut run, block);
                    } else {
                        run.push(h);
                    }
                }
                flush(&mut run, block);
                continue;
            }
            Statement::Block(ref mut b) => unemit(b, skip),
            Statement::If { ref mut accept, ref mut reject, .. } => {
                unemit(accept, skip);
                unemit(reject, skip);
            }
            Statement::Loop { ref mut body, ref mut continuing, .. } => {
                unemit(body, skip);
                unemit(continuing, skip);
            }
            Statement::Switch { ref mut cases, .. } => {
                for case in cases.iter_mut() {
                    unemit(&mut case.body, skip);
                }
            }
            _ => {}
        }
        block.push(statement, span);
    }
}

// Returns how many heap accesses couldn't be given a fixed binding.
fn fix_bindings(module: &mut Module) -> usize {
    let mut shared = None;
    let mut heaps: HashMap<Handle<GlobalVariable>, (u32, Handle<Type>)> = HashMap::new();
    for (h, g) in module.global_variables.iter() {
        if let Some(b) = g.binding {
            if g.space == AddressSpace::Uniform && b.group == 6 && b.binding == 2 {
                shared = Some(h);
            }
        }
        if let TypeInner::BindingArray { base, .. } = module.types[g.ty].inner {
            let kind = match module.types[base].inner {
                TypeInner::Sampler { .. } => 4,
                TypeInner::Image { dim, arrayed, class: ImageClass::Sampled { .. } | ImageClass::Depth { .. }, .. } => {
                    match (dim, arrayed) {
                        (ImageDimension::D2, false) => 0,
                        (ImageDimension::D2, true) => 1,
                        (ImageDimension::D3, _) => 2,
                        (ImageDimension::Cube, _) => 3,
                        _ => continue,
                    }
                }
                _ => continue,
            };
            heaps.insert(h, (kind, base));
        }
    }
    for g in module.global_variables.iter_mut().map(|(_, g)| g) {
        if let Some(b) = g.binding.as_mut() {
            if b.group == 6 {
                b.group = 0;
            }
        }
    }
    let mut fixed: HashMap<(u32, u32), Handle<GlobalVariable>> = HashMap::new();
    let mut failures = 0;
    let mut funs: Vec<*mut Function> = module.functions.iter_mut().map(|(_, f)| f as *mut _).collect();
    funs.extend(module.entry_points.iter_mut().map(|e| &mut e.function as *mut _));
    for fun in funs {
        // Safety: each function is visited once; the module's other arenas
        // are only read or appended to (global variables) meanwhile.
        let fun = unsafe { &mut *fun };
        let mut replace = Vec::new();
        for (h, e) in fun.expressions.iter() {
            let (base, index) = match *e {
                Expression::Access { base, index } => (base, index),
                _ => continue,
            };
            let Expression::GlobalVariable(heap) = fun.expressions[base] else { continue };
            let Some(&(kind, ty)) = heaps.get(&heap) else { continue };
            let slot = shared.and_then(|s| match place(fun, module, s, index) {
                Some(Place::Word(w)) => Some(w),
                _ => None,
            });
            let Some(word) = slot else {
                failures += 1;
                continue;
            };
            // 26 words per texture kind, then the samplers.
            let slot = if kind == 4 { word.wrapping_sub(104) } else { word.wrapping_sub(kind * 26) };
            if slot >= 26 {
                failures += 1;
                continue;
            }
            replace.push((h, kind, slot, ty));
        }
        let replaced: std::collections::HashSet<_> = replace.iter().map(|r| r.0).collect();
        if !replaced.is_empty() {
            unemit(&mut fun.body, &replaced);
        }
        for (h, kind, slot, ty) in replace {
            let g = *fixed.entry((kind, slot)).or_insert_with(|| {
                let (group, binding) = if kind == 4 { (2, slot) } else { (1, kind * 32 + slot) };
                eprintln!("bind {group} {binding} {kind}");
                module.global_variables.append(
                    GlobalVariable {
                        name: Some(format!("{}{slot}", ["tex2d", "tex2darray", "tex3d", "texcube", "samp"][kind as usize])),
                        space: AddressSpace::Handle,
                        binding: Some(ResourceBinding { group, binding }),
                        ty,
                        init: None,
                        memory_decorations: Default::default(),
                    },
                    Span::UNDEFINED,
                )
            });
            fun.expressions[h] = Expression::GlobalVariable(g);
        }
    }
    failures
}

// Text fixes for what naga writes but browsers' WGSL compilers reject:
// - 64-bit integers, left from the buffer-address arithmetic (all of these
//   values fit in 32 bits): `u64`/`i64` and `lu`/`li` literals;
// - f32's largest value printed rounded up, which no longer fits.
fn browser_fixes(text: &str) -> String {
    let text = text
        .replace("340282350000000000000000000000000000000f", "0x1.fffffep+127f")
        .replace("u64", "u32")
        .replace("i64", "i32");
    let bytes = text.as_bytes();
    let mut out = String::with_capacity(text.len());
    let mut i = 0;
    while i < bytes.len() {
        let c = bytes[i];
        if c == b'l' && i > 0 && bytes[i - 1].is_ascii_digit() && i + 1 < bytes.len() && matches!(bytes[i + 1], b'u' | b'i') {
            // Check the digits are a literal, not the end of an identifier.
            let mut j = i;
            while j > 0 && bytes[j - 1].is_ascii_digit() {
                j -= 1;
            }
            if j == 0 || !(bytes[j - 1].is_ascii_alphanumeric() || bytes[j - 1] == b'_') {
                i += 1;
                continue;
            }
        }
        out.push(c as char);
        i += 1;
    }
    out
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!("usage: spv2wgsl <in.spv> <out.wgsl>");
        std::process::exit(2);
    }
    let data = std::fs::read(&args[1]).expect("read");
    let mut module = match naga::front::spv::parse_u8_slice(&data, &Default::default()) {
        Ok(m) => m,
        Err(e) => {
            eprintln!("spv: {e}");
            std::process::exit(1);
        }
    };
    lower(&mut module);
    let unbound = fix_bindings(&mut module);
    if unbound > 0 {
        eprintln!("unbound: {unbound} heap accesses with no constant slot");
    }
    naga::compact::compact(&mut module, naga::compact::KeepUnused::No);
    let info = match naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    )
    .validate(&module)
    {
        Ok(i) => i,
        Err(e) => {
            eprintln!("validate: {:?}", e.into_inner());
            std::process::exit(1);
        }
    };
    match naga::back::wgsl::write_string(&module, &info, naga::back::wgsl::WriterFlags::empty()) {
        Ok(text) => {
            // Xenos shaders sample textures inside data-dependent control flow
            // (the dispatch loops), which WGSL's uniformity analysis rejects.
            let text = format!("diagnostic(off, derivative_uniformity);\n{}", browser_fixes(&text));
            std::fs::write(&args[2], text).expect("write");
        }
        Err(e) => {
            eprintln!("wgsl: {e}");
            std::process::exit(1);
        }
    }
}
