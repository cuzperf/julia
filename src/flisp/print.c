extern void *memrchr(const void *s, int c, size_t n);

/* === 基本输出函数 === */

/* 输出单个字符并更新水平位置 */
static void outc(fl_context_t *fl_ctx, char c, ios_t *f)
{
    ios_putc(c, f);
    if (c == '\n')
        fl_ctx->HPOS = 0;
    else
        fl_ctx->HPOS++;
}
/* 输出字符串并更新水平位置 */
static void outs(fl_context_t *fl_ctx, const char *s, ios_t *f)
{
    ios_puts(s, f);
    fl_ctx->HPOS += u8_strwidth(s);
}
/* 输出指定长度的字符串并更新水平位置 */
static void outsn(fl_context_t *fl_ctx, const char *s, ios_t *f, size_t n)
{
    ios_write(f, s, n);
    fl_ctx->HPOS += u8_strwidth(s);
}
/* 输出换行和缩进，若缩进过大则回退到左边距 */
static int outindent(fl_context_t *fl_ctx, int n, ios_t *f)
{
    // move back to left margin if we get too indented
    if (n > fl_ctx->SCR_WIDTH-12)
        n = 2;
    int n0 = n;
    ios_putc('\n', f);
    fl_ctx->VPOS++;
    fl_ctx->HPOS = n;
    while (n >= 8) {
        ios_putc('\t', f);
        n -= 8;
    }
    while (n) {
        ios_putc(' ', f);
        n--;
    }
    return n0;
}

/* 打印单个字符（公共接口） */
void fl_print_chr(fl_context_t *fl_ctx, char c, ios_t *f)
{
    outc(fl_ctx, c, f);
}

/* 打印字符串（公共接口） */
void fl_print_str(fl_context_t *fl_ctx, const char *s, ios_t *f)
{
    outs(fl_ctx, s, f);
}

/* === 共享结构检测与循环引用处理 === */

/* 遍历表达式树，标记共享结构以用于循环引用检测 */
void print_traverse(fl_context_t *fl_ctx, value_t v)
{
    value_t *bp;
    while (iscons(v)) {
        if (ismarked(fl_ctx, v)) {
            bp = (value_t*)ptrhash_bp(&fl_ctx->printconses, (void*)v);
            if (*bp == (value_t)HT_NOTFOUND)
                *bp = fixnum(fl_ctx->printlabel++);
            return;
        }
        mark_cons(fl_ctx, v);
        print_traverse(fl_ctx, car_(v));
        v = cdr_(v);
    }
    if (!ismanaged(fl_ctx, v) || issymbol(v))
        return;
    if (ismarked(fl_ctx, v)) {
        bp = (value_t*)ptrhash_bp(&fl_ctx->printconses, (void*)v);
        if (*bp == (value_t)HT_NOTFOUND)
            *bp = fixnum(fl_ctx->printlabel++);
        return;
    }
    if (isvector(v)) {
        if (vector_size(v) > 0)
            mark_cons(fl_ctx, v);
        unsigned int i;
        for(i=0; i < vector_size(v); i++)
            print_traverse(fl_ctx, vector_elt(v,i));
    }
    else if (iscprim(v)) {
        mark_cons(fl_ctx, v);
    }
    else if (isclosure(v)) {
        mark_cons(fl_ctx, v);
        function_t *f = (function_t*)ptr(v);
        print_traverse(fl_ctx, f->bcode);
        print_traverse(fl_ctx, f->vals);
        print_traverse(fl_ctx, f->env);
    }
    else {
        assert(iscvalue(v));
        cvalue_t *cv = (cvalue_t*)ptr(v);
        // don't consider shared references to ""
        if (!cv_isstr(fl_ctx, cv) || cv_len(cv)!=0)
            mark_cons(fl_ctx, v);
        fltype_t *t = cv_class(cv);
        if (t->vtable != NULL && t->vtable->print_traverse != NULL)
            t->vtable->print_traverse(fl_ctx, v);
    }
}

/* 打印符号名称，必要时使用|...|转义（包含特殊字符时） */
static void print_symbol_name(fl_context_t *fl_ctx, ios_t *f, char *name)
{
    int i, escape=0, charescape=0;

    if ((name[0] == '\0') ||
        (name[0] == '.' && name[1] == '\0') ||
        (name[0] == '#') ||
        isnumtok(fl_ctx, name, NULL))
        escape = 1;
    i=0;
    while (name[i]) {
        if (!symchar(name[i])) {
            escape = 1;
            if (name[i]=='|' || name[i]=='\\') {
                charescape = 1;
                break;
            }
        }
        i++;
    }
    if (escape) {
        if (charescape) {
            outc(fl_ctx, '|', f);
            i=0;
            while (name[i]) {
                if (name[i]=='|' || name[i]=='\\')
                    outc(fl_ctx, '\\', f);
                outc(fl_ctx, name[i], f);
                i++;
            }
            outc(fl_ctx, '|', f);
        }
        else {
            outc(fl_ctx, '|', f);
            outs(fl_ctx, name, f);
            outc(fl_ctx, '|', f);
        }
    }
    else {
        outs(fl_ctx, name, f);
    }
}

/*
  The following implements a simple pretty-printing algorithm. This is
  an unlimited-width approach that doesn't require an extra pass.
  It uses some heuristics to guess whether an expression is "small",
  and avoids wrapping symbols across lines. The result is high
  performance and nice output for typical code. Quality is poor for
  pathological or deeply-nested expressions, but those are difficult
  to print anyway.
*/
/* === 美观打印辅助函数 === */

#define SMALL_STR_LEN 20
/* 判断表达式是否"微小"（单个符号/字符串长度小于20、或数字/布尔值等） */
static inline int tinyp(fl_context_t *fl_ctx, value_t v)
{
    if (issymbol(v))
        return (u8_strwidth(symbol_name(fl_ctx, v)) < SMALL_STR_LEN);
    if (fl_isstring(fl_ctx, v))
        return (cv_len((cvalue_t*)ptr(v)) < SMALL_STR_LEN);
    return (isfixnum(v) || isbuiltin(v) || v==fl_ctx->F || v==fl_ctx->T || v==fl_ctx->NIL ||
            v == fl_ctx->FL_EOF);
}

/* 判断表达式是否"小"（tiny的扩展，包含数字和简单列表） */
static int smallp(fl_context_t *fl_ctx, value_t v)
{
    if (tinyp(fl_ctx, v)) return 1;
    if (fl_isnumber(fl_ctx, v)) return 1;
    if (iscons(v)) {
        if (tinyp(fl_ctx, car_(v)) && (tinyp(fl_ctx, cdr_(v)) ||
                               (iscons(cdr_(v)) && tinyp(fl_ctx, car_(cdr_(v))) &&
                                cdr_(cdr_(v))==fl_ctx->NIL)))
            return 1;
        return 0;
    }
    if (isvector(v)) {
        size_t s = vector_size(v);
        return (s == 0 || (tinyp(fl_ctx, vector_elt(v,0)) &&
                           (s == 1 || (s == 2 &&
                                       tinyp(fl_ctx, vector_elt(v,1))))));
    }
    return 0;
}

/* 特殊形式（lambda、try-catch、defines、for等）使用2空格缩进 */
static int specialindent(fl_context_t *fl_ctx, value_t head)
{
    // indent these forms 2 spaces, not lined up with the first argument
    if (head == fl_ctx->LAMBDA || head == fl_ctx->TRYCATCH || head == fl_ctx->definesym ||
        head == fl_ctx->defmacrosym || head == fl_ctx->forsym)
        return 2;
    return -1;
}

/* 低成本估算表达式的显示宽度（符号返回实际宽度，其余返回-1） */
static int lengthestimate(fl_context_t *fl_ctx, value_t v)
{
    // get the width of an expression if we can do so cheaply
    if (issymbol(v))
        return u8_strwidth(symbol_name(fl_ctx, v));
    return -1;
}

/* 检查列表的所有子表达式是否都"小"，最多检查25个元素 */
static int allsmallp(fl_context_t *fl_ctx, value_t v)
{
    int n = 1;
    while (iscons(v)) {
        if (!smallp(fl_ctx, car_(v)))
            return 0;
        v = cdr_(v);
        n++;
        if (n > 25)
            return n;
    }
    return n;
}

/* 某些特殊形式（如for）总是在第3个元素后缩进 */
static int indentafter3(fl_context_t *fl_ctx, value_t head, value_t v)
{
    // for certain X always indent (X a b c) after b
    return ((head == fl_ctx->forsym) && !allsmallp(fl_ctx, cdr_(v)));
}

/* 某些特殊形式（如define、defmacro）总是在第2个元素后缩进 */
static int indentafter2(fl_context_t *fl_ctx, value_t head, value_t v)
{
    // for certain X always indent (X a b) after a
    return ((head == fl_ctx->definesym || head == fl_ctx->defmacrosym) &&
            !allsmallp(fl_ctx, cdr_(v)));
}

/* 特殊形式（if等）在每个子表达式前缩进，除非所有子表达式都"小" */
static int indentevery(fl_context_t *fl_ctx, value_t v)
{
    // indent before every subform of a special form, unless every
    // subform is "small"
    value_t c = car_(v);
    if (c == fl_ctx->LAMBDA || c == fl_ctx->setqsym)
        return 0;
    if (c == fl_ctx->IF) // TODO: others
        return !allsmallp(fl_ctx, cdr_(v));
    return 0;
}

/* 若列表长度超过9且所有元素都"小"，切换到块缩进模式 */
static int blockindent(fl_context_t *fl_ctx, value_t v)
{
    // in this case we switch to block indent mode, where the head
    // is no longer considered special:
    // (a b c d e
    //  f g h i j)
    return (allsmallp(fl_ctx, v) > 9);
}

/* 打印列表（括号对），支持美观打印、引用语法糖等 */
static void print_pair(fl_context_t *fl_ctx, ios_t *f, value_t v)
{
    value_t cd;
    char *op = NULL;
    if (iscons(cdr_(v)) && cdr_(cdr_(v)) == fl_ctx->NIL &&
        !ptrhash_has(&fl_ctx->printconses, (void*)cdr_(v)) &&
        (((car_(v) == fl_ctx->QUOTE)     && (op = "'"))  ||
         ((car_(v) == fl_ctx->BACKQUOTE) && (op = "`"))  ||
         ((car_(v) == fl_ctx->COMMA)     && (op = ","))  ||
         ((car_(v) == fl_ctx->COMMAAT)   && (op = ",@")) ||
         ((car_(v) == fl_ctx->COMMADOT)  && (op = ",.")))) {
        // special prefix syntax
        unmark_cons(fl_ctx, v);
        unmark_cons(fl_ctx, cdr_(v));
        outs(fl_ctx, op, f);
        fl_print_child(fl_ctx, f, car_(cdr_(v)));
        return;
    }
    int startpos = fl_ctx->HPOS;
    outc(fl_ctx, '(', f);
    int newindent=fl_ctx->HPOS, blk=blockindent(fl_ctx, v);
    int lastv, n=0, si, ind=0, est, always=0, nextsmall, thistiny;
    if (!blk) always = indentevery(fl_ctx, v);
    value_t head = car_(v);
    int after3 = indentafter3(fl_ctx, head, v);
    int after2 = indentafter2(fl_ctx, head, v);
    int n_unindented = 1;
    while (1) {
        cd = cdr_(v);
        if (fl_ctx->print_length >= 0 && n >= fl_ctx->print_length && cd!=fl_ctx->NIL) {
            outsn(fl_ctx, "...)", f, 4);
            break;
        }
        lastv = fl_ctx->VPOS;
        unmark_cons(fl_ctx, v);
        fl_print_child(fl_ctx, f, car_(v));
        if (!iscons(cd) || ptrhash_has(&fl_ctx->printconses, (void*)cd)) {
            if (cd != fl_ctx->NIL) {
                outsn(fl_ctx, " . ", f, 3);
                fl_print_child(fl_ctx, f, cd);
            }
            outc(fl_ctx, ')', f);
            break;
        }

        if (!fl_ctx->print_pretty ||
            ((head == fl_ctx->LAMBDA) && n == 0)) {
            // never break line before lambda-list
            ind = 0;
        }
        else {
            est = lengthestimate(fl_ctx, car_(cd));
            nextsmall = smallp(fl_ctx, car_(cd));
            thistiny = tinyp(fl_ctx, car_(v));
            ind = (((fl_ctx->VPOS > lastv) ||
                    (fl_ctx->HPOS>fl_ctx->SCR_WIDTH/2 && !nextsmall && !thistiny && n>0)) ||

                   (fl_ctx->HPOS > fl_ctx->SCR_WIDTH-4) ||

                   (est!=-1 && (fl_ctx->HPOS+est > fl_ctx->SCR_WIDTH-2)) ||

                   ((head == fl_ctx->LAMBDA) && !nextsmall) ||

                   (n > 0 && always) ||

                   (n == 2 && after3) ||
                   (n == 1 && after2) ||

                   (n_unindented >= 3 && !nextsmall) ||

                   (n == 0 && !smallp(fl_ctx, head)));
        }

        if (ind) {
            newindent = outindent(fl_ctx, newindent, f);
            n_unindented = 1;
        }
        else {
            n_unindented++;
            outc(fl_ctx, ' ', f);
            if (n==0) {
                // set indent level after printing head
                si = specialindent(fl_ctx, head);
                if (si != -1)
                    newindent = startpos + si;
                else if (!blk)
                    newindent = fl_ctx->HPOS;
            }
        }
        n++;
        v = cd;
    }
}

static void cvalue_print(fl_context_t *fl_ctx, ios_t *f, value_t v);

/* 为共享结构打印循环引用前缀（#n#或#n=），若已标记则返回1 */
static int print_circle_prefix(fl_context_t *fl_ctx, ios_t *f, value_t v)
{
    value_t label;
    char buf[64];
    char *str;
    if ((label=(value_t)ptrhash_get(&fl_ctx->printconses, (void*)v)) !=
        (value_t)HT_NOTFOUND) {
        if (!ismarked(fl_ctx, v)) {
            //fl_ctx->HPOS+=ios_printf(f, "#%ld#", numval(label));
            outc(fl_ctx, '#', f);
            str = uint2str(buf, sizeof(buf)-1, numval(label), 10);
            outs(fl_ctx, str, f);
            outc(fl_ctx, '#', f);
            return 1;
        }
        //fl_ctx->HPOS+=ios_printf(f, "#%ld=", numval(label));
        outc(fl_ctx, '#', f);
        str = uint2str(buf, sizeof(buf)-1, numval(label), 10);
        outs(fl_ctx, str, f);
        outc(fl_ctx, '=', f);
    }
    if (ismanaged(fl_ctx, v))
        unmark_cons(fl_ctx, v);
    return 0;
}

/* === 值打印 === */

/* 递归打印一个值（核心函数），处理各种类型：数字、符号、函数、向量、cvalue等 */
void fl_print_child(fl_context_t *fl_ctx, ios_t *f, value_t v)
{
    char *name, *str;
    char buf[64];
    if (fl_ctx->print_level >= 0 && fl_ctx->P_LEVEL >= fl_ctx->print_level &&
        (iscons(v) || isvector(v) || isclosure(v))) {
        outc(fl_ctx, '#', f);
        return;
    }
    fl_ctx->P_LEVEL++;

    switch (tag(v)) {
    case TAG_NUM :
    case TAG_NUM1: //fl_ctx->HPOS+=ios_printf(f, "%ld", numval(v)); break;
        str = uint2str(&buf[1], sizeof(buf)-1, labs(numval(v)), 10);
        if (numval(v)<0)
            *(--str) = '-';
        outs(fl_ctx, str, f);
        break;
    case TAG_SYM:
        name = symbol_name(fl_ctx, v);
        if (fl_ctx->print_princ)
            outs(fl_ctx, name, f);
        else if (ismanaged(fl_ctx, v)) {
            outsn(fl_ctx, "#:", f, 2);
            outs(fl_ctx, name, f);
        }
        else
            print_symbol_name(fl_ctx, f, name);
        break;
    case TAG_FUNCTION:
        if (v == fl_ctx->T) {
            outsn(fl_ctx, "#t", f, 2);
        }
        else if (v == fl_ctx->F) {
            outsn(fl_ctx, "#f", f, 2);
        }
        else if (v == fl_ctx->NIL) {
            outsn(fl_ctx, "()", f, 2);
        }
        else if (v == fl_ctx->FL_EOF) {
            outsn(fl_ctx, "#<eof>", f, 6);
        }
        else if (isbuiltin(v)) {
            if (!fl_ctx->print_princ)
                outsn(fl_ctx, "#.", f, 2);
            outs(fl_ctx, builtin_names[uintval(v)], f);
        }
        else {
            assert(isclosure(v));
            if (!fl_ctx->print_princ) {
                if (print_circle_prefix(fl_ctx, f, v)) break;
                function_t *fn = (function_t*)ptr(v);
                outs(fl_ctx, "#fn(", f);
                char *data = (char*)cvalue_data(fn->bcode);
                size_t i, sz = cvalue_len(fn->bcode);
                for(i=0; i < sz; i++) data[i] += 48;
                fl_print_child(fl_ctx, f, fn->bcode);
                for(i=0; i < sz; i++) data[i] -= 48;
                outc(fl_ctx, ' ', f);
                fl_print_child(fl_ctx, f, fn->vals);
                if (fn->env != fl_ctx->NIL) {
                    outc(fl_ctx, ' ', f);
                    fl_print_child(fl_ctx, f, fn->env);
                }
                if (fn->name != fl_ctx->LAMBDA) {
                    outc(fl_ctx, ' ', f);
                    fl_print_child(fl_ctx, f, fn->name);
                }
                outc(fl_ctx, ')', f);
            }
            else {
                outs(fl_ctx, "#<function>", f);
            }
        }
        break;
    case TAG_CVALUE:
    case TAG_CPRIM:
        if (v == UNBOUND) { outs(fl_ctx, "#<undefined>", f); break; }
        JL_FALLTHROUGH;
    case TAG_VECTOR:
    case TAG_CONS:
        if (print_circle_prefix(fl_ctx, f, v)) break;
        if (isvector(v)) {
            outc(fl_ctx, '[', f);
            int newindent = fl_ctx->HPOS, est;
            int i, sz = vector_size(v);
            for(i=0; i < sz; i++) {
                if (fl_ctx->print_length >= 0 && i >= fl_ctx->print_length && i < sz-1) {
                    outsn(fl_ctx, "...", f, 3);
                    break;
                }
                fl_print_child(fl_ctx, f, vector_elt(v,i));
                if (i < sz-1) {
                    if (!fl_ctx->print_pretty) {
                        outc(fl_ctx, ' ', f);
                    }
                    else {
                        est = lengthestimate(fl_ctx, vector_elt(v,i+1));
                        if (fl_ctx->HPOS > fl_ctx->SCR_WIDTH-4 ||
                            (est!=-1 && (fl_ctx->HPOS+est > fl_ctx->SCR_WIDTH-2)) ||
                            (fl_ctx->HPOS > fl_ctx->SCR_WIDTH/2 &&
                             !smallp(fl_ctx, vector_elt(v,i+1)) &&
                             !tinyp(fl_ctx, vector_elt(v,i))))
                            newindent = outindent(fl_ctx, newindent, f);
                        else
                            outc(fl_ctx, ' ', f);
                    }
                }
            }
            outc(fl_ctx, ']', f);
            break;
        }
        if (iscvalue(v) || iscprim(v))
            cvalue_print(fl_ctx, f, v);
        else
            print_pair(fl_ctx, f, v);
        break;
    }
    fl_ctx->P_LEVEL--;
}

/* 打印字符串，使用"..."引号包围，并对特殊字符进行转义 */
static void print_string(fl_context_t *fl_ctx, ios_t *f, char *str, size_t sz)
{
    char buf[512];
    size_t i = 0;
    uint8_t c;
    static const char hexdig[] = "0123456789abcdef";

    outc(fl_ctx, '"', f);
    if (!u8_isvalid(str, sz)) {
        // alternate print algorithm that preserves data if it's not UTF-8
        for(i=0; i < sz; i++) {
            c = str[i];
            if (c == '\\')
                outsn(fl_ctx, "\\\\", f, 2);
            else if (c == '"')
                outsn(fl_ctx, "\\\"", f, 2);
            else if (c >= 32 && c < 0x7f)
                outc(fl_ctx, c, f);
            else {
                outsn(fl_ctx, "\\x", f, 2);
                outc(fl_ctx, hexdig[c>>4], f);
                outc(fl_ctx, hexdig[c&0xf], f);
            }
        }
    }
    else {
        while (i < sz) {
            size_t n = u8_escape(buf, sizeof(buf), str, &i, sz, "\"", 0);
            outsn(fl_ctx, buf, f, n-1);
        }
    }
    outc(fl_ctx, '"', f);
}

static numerictype_t sym_to_numtype(fl_context_t *fl_ctx, value_t type);
#ifndef _OS_WINDOWS_
#define __USE_GNU
#include <dlfcn.h>
#undef __USE_GNU
#endif

#define sign_bit(r) ((*(int64_t*)&(r)) & BIT63)
#define DFINITE(d) (((*(int64_t*)&(d))&0x7ff0000000000000LL)!=0x7ff0000000000000LL)

// 'weak' means we don't need to accurately reproduce the type, so
// for example #int32(0) can be printed as just 0. this is used when
// printing in a context where a type is already implied, e.g. inside
// an array.
/* 打印cvalue的数据内容，weak模式可省略类型标签 */
static void cvalue_printdata(fl_context_t *fl_ctx, ios_t *f, void *data,
                             size_t len, value_t type, int weak)
{
    if (type == fl_ctx->bytesym) {
        unsigned char ch = *(unsigned char*)data;
        if (fl_ctx->print_princ)
            outc(fl_ctx, ch, f);
        else if (weak)
            fl_ctx->HPOS+=ios_printf(f, "0x%hhx", ch);
        else
            fl_ctx->HPOS+=ios_printf(f, "#byte(0x%hhx)", ch);
    }
    else if (type == fl_ctx->wcharsym) {
        uint32_t wc = *(uint32_t*)data;
        char seq[8];
        size_t nb = u8_toutf8(seq, sizeof(seq), &wc, 1);
        seq[nb] = '\0';
        if (fl_ctx->print_princ) {
            // TODO: better multibyte handling
            outs(fl_ctx, seq, f);
        }
        else {
            outsn(fl_ctx, "#\\", f, 2);
            if      (wc == 0x00) outsn(fl_ctx, "nul", f, 3);
            else if (wc == 0x07) outsn(fl_ctx, "alarm", f, 5);
            else if (wc == 0x08) outsn(fl_ctx, "backspace", f, 9);
            else if (wc == 0x09) outsn(fl_ctx, "tab", f, 3);
            else if (wc == 0x0A) outsn(fl_ctx, "linefeed", f, 8);
            //else if (wc == 0x0A) outsn(fl_ctx, "newline", f, 7);
            else if (wc == 0x0B) outsn(fl_ctx, "vtab", f, 4);
            else if (wc == 0x0C) outsn(fl_ctx, "page", f, 4);
            else if (wc == 0x0D) outsn(fl_ctx, "return", f, 6);
            else if (wc == 0x1B) outsn(fl_ctx, "esc", f, 3);
            else if (wc == 0x20) outsn(fl_ctx, "space", f, 5);
            else if (wc == 0x7F) outsn(fl_ctx, "delete", f, 6);
            else if (iswprint(wc)) outs(fl_ctx, seq, f);
            else fl_ctx->HPOS+=ios_printf(f, "x%04x", (int)wc);
        }
    }
    else if (type == fl_ctx->floatsym || type == fl_ctx->doublesym) {
        char buf[64];
        double d;
        if (type == fl_ctx->floatsym) { d = (double)*(float*)data; }
        else { d = *(double*)data; }
        if (!DFINITE(d)) {
            char *rep;
            if (d != d)
                rep = (char*)(sign_bit(d) ? "-nan.0" : "+nan.0");
            else
                rep = (char*)(sign_bit(d) ? "-inf.0" : "+inf.0");
            if (type == fl_ctx->floatsym && !fl_ctx->print_princ && !weak)
                fl_ctx->HPOS+=ios_printf(f, "#%s(%s)", symbol_name(fl_ctx, type), rep);
            else
                outs(fl_ctx, rep, f);
        }
        else if (d == 0) {
            if (sign_bit(d))
                outsn(fl_ctx, "-0.0", f, 4);
            else
                outsn(fl_ctx, "0.0", f, 3);
            if (type == fl_ctx->floatsym && !fl_ctx->print_princ && !weak)
                outc(fl_ctx, 'f', f);
        }
        else {
            double ad = d < 0 ? -d : d;
            if ((long)d == d && ad < 1e6 && ad >= 1e-4) {
                snprintf(buf, sizeof(buf), "%g", d);
            }
            else {
                if (type == fl_ctx->floatsym)
                    snprintf(buf, sizeof(buf), "%.8g", d);
                else
                    snprintf(buf, sizeof(buf), "%.16g", d);
            }
            int hasdec = (strpbrk(buf, ".eE") != NULL);
            outs(fl_ctx, buf, f);
            if (!hasdec) outsn(fl_ctx, ".0", f, 2);
            if (type == fl_ctx->floatsym && !fl_ctx->print_princ && !weak)
                outc(fl_ctx, 'f', f);
        }
    }
    else if (type == fl_ctx->uint64sym
#ifdef _P64
             || type == fl_ctx->sizesym
#endif
             ) {
        uint64_t ui64 = *(uint64_t*)data;
        if (weak || fl_ctx->print_princ)
            fl_ctx->HPOS += ios_printf(f, "%llu", ui64);
        else
            fl_ctx->HPOS += ios_printf(f, "#%s(%llu)", symbol_name(fl_ctx, type), ui64);
    }
    else if (issymbol(type)) {
        // handle other integer prims. we know it's smaller than uint64
        // at this point, so int64 is big enough to capture everything.
        numerictype_t nt = sym_to_numtype(fl_ctx, type);
        if (nt == N_NUMTYPES) {
            // These states should be context independent.
            static size_t (*volatile jl_static_print)(ios_t*, void*) = NULL;
            static volatile int init = 0;
            // XXX: use uv_once
            if (init == 0) {
#if defined(RTLD_SELF)
                jl_static_print = (size_t (*)(ios_t*, void*))
                    (uintptr_t)dlsym(RTLD_SELF, "ijl_static_show");
#elif defined(RTLD_DEFAULT)
                jl_static_print = (size_t (*)(ios_t*, void*))
                    (uintptr_t)dlsym(RTLD_DEFAULT, "ijl_static_show");
#elif defined(_OS_WINDOWS_)
                HMODULE handle;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCWSTR)(&cvalue_printdata),
                                       &handle)) {
                    jl_static_print = (size_t (*)(ios_t*, void*))
                        (uintptr_t)GetProcAddress(handle, "ijl_static_show");
                }
#endif
                init = 1;
            }
            if (jl_static_print != NULL && fl_ctx->jl_sym == type) {
                fl_ctx->HPOS += ios_printf(f, "#<julia: ");
                fl_ctx->HPOS += jl_static_print(f, *(void**)data);
                fl_ctx->HPOS += ios_printf(f, ">");
            }
            else
                fl_ctx->HPOS += ios_printf(f, "#<%s>", symbol_name(fl_ctx, type));
        }
        else {
            int64_t i64 = conv_to_int64(data, nt);
            if (weak || fl_ctx->print_princ)
                fl_ctx->HPOS += ios_printf(f, "%lld", i64);
            else
                fl_ctx->HPOS += ios_printf(f, "#%s(%lld)", symbol_name(fl_ctx, type), i64);
        }
    }
    else if (iscons(type)) {
        if (car_(type) == fl_ctx->arraysym) {
            value_t eltype = car(fl_ctx, cdr_(type));
            size_t cnt, elsize;
            if (iscons(cdr_(cdr_(type)))) {
                cnt = tosize(fl_ctx, car_(cdr_(cdr_(type))), "length");
                elsize = cnt ? len/cnt : 0;
            }
            else {
                // incomplete array type
                int junk;
                elsize = ctype_sizeof(fl_ctx, eltype, &junk);
                cnt = elsize ? len/elsize : 0;
            }
            if (eltype == fl_ctx->bytesym) {
                if (fl_ctx->print_princ) {
                    ios_write(f, (char*)data, len);
                    /*
                    char *nl = memrchr(data, '\n', len);
                    if (nl)
                        fl_ctx->HPOS = u8_strwidth(nl+1);
                    else
                        fl_ctx->HPOS += u8_strwidth(data);
                    */
                }
                else {
                    print_string(fl_ctx, f, (char*)data, len);
                }
                return;
            }
            else if (eltype == fl_ctx->wcharsym) {
                // TODO wchar
            }
            else {
            }
            size_t i;
            if (!weak) {
                if (eltype == fl_ctx->uint8sym) {
                    outsn(fl_ctx, "#vu8(", f, 5);
                }
                else {
                    outsn(fl_ctx, "#array(", f, 7);
                    fl_print_child(fl_ctx, f, eltype);
                    if (cnt > 0)
                        outc(fl_ctx, ' ', f);
                }
            }
            else {
                outc(fl_ctx, '[', f);
            }
            for(i=0; i < cnt; i++) {
                if (i > 0)
                    outc(fl_ctx, ' ', f);
                cvalue_printdata(fl_ctx, f, data, elsize, eltype, 1);
                data = (char *)data + elsize;
            }
            if (!weak)
                outc(fl_ctx, ')', f);
            else
                outc(fl_ctx, ']', f);
        }
    }
}

/* 打印cvalue：内置函数指针、自定义vtable打印、或通用数据打印 */
static void cvalue_print(fl_context_t *fl_ctx, ios_t *f, value_t v)
{
    cvalue_t *cv = (cvalue_t*)ptr(v);
    void *data = cptr(v);
    value_t label;

    if (cv_class(cv) == fl_ctx->builtintype) {
        void *fptr = *(void**)data;
        label = (value_t)ptrhash_get(&fl_ctx->reverse_dlsym_lookup_table, cv);
        if (label == (value_t)HT_NOTFOUND) {
            fl_ctx->HPOS += ios_printf(f, "#<builtin @0x%08zx>", (size_t)fptr);
        }
        else {
            if (fl_ctx->print_princ) {
                outs(fl_ctx, symbol_name(fl_ctx, label), f);
            }
            else {
                outsn(fl_ctx, "#fn(", f, 4);
                outs(fl_ctx, symbol_name(fl_ctx, label), f);
                outc(fl_ctx, ')', f);
            }
        }
    }
    else if (cv_class(cv)->vtable != NULL &&
             cv_class(cv)->vtable->print != NULL) {
        cv_class(cv)->vtable->print(fl_ctx, v, f);
    }
    else {
        value_t type = cv_type(cv);
        size_t len = iscprim(v) ? cv_class(cv)->size : cv_len(cv);
        cvalue_printdata(fl_ctx, f, data, len, type, 0);
    }
}

/* 从符号中读取纸张宽度设置 */
static void set_print_width(fl_context_t *fl_ctx)
{
    value_t pw = symbol_value(fl_ctx->printwidthsym);
    if (!isfixnum(pw)) return;
    fl_ctx->SCR_WIDTH = numval(pw);
}

/* === 公共打印接口 === */

/* 主打印函数：设置打印参数，遍历检测共享结构，然后递归打印值 */
void fl_print(fl_context_t *fl_ctx, ios_t *f, value_t v)
{
    fl_ctx->print_pretty = (symbol_value(fl_ctx->printprettysym) != fl_ctx->F);
    if (fl_ctx->print_pretty)
        set_print_width(fl_ctx);
    fl_ctx->print_princ = (symbol_value(fl_ctx->printreadablysym) == fl_ctx->F);

    value_t pl = symbol_value(fl_ctx->printlengthsym);
    if (isfixnum(pl)) fl_ctx->print_length = numval(pl);
    else fl_ctx->print_length = -1;
    pl = symbol_value(fl_ctx->printlevelsym);
    if (isfixnum(pl)) fl_ctx->print_level = numval(pl);
    else fl_ctx->print_level = -1;
    fl_ctx->P_LEVEL = 0;

    fl_ctx->printlabel = 0;
    print_traverse(fl_ctx, v);
    fl_ctx->HPOS = fl_ctx->VPOS = 0;

    fl_print_child(fl_ctx, f, v);

    if (fl_ctx->print_level >= 0 || fl_ctx->print_length >= 0) {
        memset(fl_ctx->consflags, 0, 4*bitvector_nwords(fl_ctx->heapsize/sizeof(cons_t)));
    }

    if ((iscons(v) || isvector(v) || isfunction(v) || iscvalue(v)) &&
        !fl_isstring(fl_ctx, v) && v!=fl_ctx->T && v!=fl_ctx->F && v!=fl_ctx->NIL) {
        htable_reset(&fl_ctx->printconses, 32);
    }
}

/* 初始化打印子系统 */
void fl_print_init(fl_context_t *fl_ctx)
{
    htable_new(&fl_ctx->printconses, 32);
    fl_ctx->SCR_WIDTH = 80;
    fl_ctx->HPOS = 0;
}

/* ============================================================
 * Julia flisp 内存结构可视化工具（完整修正版）
 * 修正点：
 * 1. builtin 立即数判断先于 ptr(v) 解引用，避免 segfault
 * 2. OP_RPLACA/OP_RPLACD → OP_SETCAR/OP_SETCDR（匹配 opcodes.h）
 * 3. sym->name 是柔性数组，检查 name[0] 而非 sym->name
 * 4. gensym_t 与 symbol_t 分支完全分离
 * 5. 基于 opcodes.h 的完整 builtin 名称映射
 * 插入到 flisp.c 末尾，并在 flisp.h 中声明
 * ============================================================ */

#define MAX_VISITED 256

typedef struct {
    void *addrs[MAX_VISITED];
    int count;
} visited_set_t;

static int is_visited(visited_set_t *vis, void *p) {
    if (!p) return 0;
    for (int i = 0; i < vis->count; i++)
        if (vis->addrs[i] == p) return 1;
    return 0;
}

static void mark_visited(visited_set_t *vis, void *p) {
    if (p && vis->count < MAX_VISITED) vis->addrs[vis->count++] = p;
}

static void print_indent(ios_t *f, int depth) {
    for (int i = 0; i < depth; i++) ios_printf(f, "  ");
}

static void _fl_print_mem_impl(fl_context_t *fl_ctx, ios_t *f, value_t v,
                                int depth, int max_depth, visited_set_t *vis);

/* 公共接口 */
void fl_print_memory(fl_context_t *fl_ctx, ios_t *f, value_t v, int max_depth) {
    visited_set_t vis = {0};
    ios_printf(f, "value_t raw=0x%lx\n", (unsigned long)v);
    _fl_print_mem_impl(fl_ctx, f, v, 0, max_depth, &vis);
    ios_printf(f, "\n");
    ios_flush(f);
}

/* 快速摘要 */
void fl_print_mem_summary(fl_context_t *fl_ctx, ios_t *f, value_t v) {
    if (isfixnum(v)) {
        ios_printf(f, "FIXNUM(%ld)", (long)numval(v));
    } else if (v == fl_ctx->NIL) {
        ios_printf(f, "NIL");
    } else if (v == UNBOUND) {
        ios_printf(f, "UNBOUND/FWD");
    } else {
        uint32_t t = tag(v);
        switch (t) {
            case TAG_CPRIM: {
                cprim_t *cp = (cprim_t*)ptr(v);
                ios_printf(f, "CPRIM@%p", (void*)cp);
                break;
            }
            case TAG_FUNCTION: {
                if (tinyp(fl_ctx, v)) {
                    ios_printf(f, "BUILTIN(op=%u)", (unsigned)uintval(v));
                } else {
                    function_t *fn = (function_t*)ptr(v);
                    ios_printf(f, "FUNC@%p", (void*)fn);
                }
                break;
            }
            case TAG_VECTOR:
                ios_printf(f, "VEC@%p[len=%zu]", ptr(v), vector_size(v));
                break;
            case TAG_NUM1:
                ios_printf(f, "NUM1(0x%lx)", (unsigned long)v);
                break;
            case TAG_CVALUE: {
                cvalue_t *cv = (cvalue_t*)ptr(v);
                ios_printf(f, "CVALUE@%p", (void*)cv);
                break;
            }
            case TAG_SYM: {
                if (isgensym(fl_ctx, v)) {
                    gensym_t *gs = (gensym_t*)ptr(v);
                    ios_printf(f, "GENSYM#%u", gs ? gs->id : 0);
                } else {
                    symbol_t *sym = (symbol_t*)ptr(v);
                    ios_printf(f, "SYM(%s)", (sym && sym->name[0]) ? sym->name : "?");
                }
                break;
            }
            case TAG_CONS:
                ios_printf(f, "CONS@%p", ptr(v));
                break;
            default:
                ios_printf(f, "UNKNOWN(tag=%d,raw=0x%lx)", t, (unsigned long)v);
        }
    }
    ios_flush(f);
}

/* 核心递归打印 */
static void _fl_print_mem_impl(fl_context_t *fl_ctx, ios_t *f, value_t v,
                                int depth, int max_depth, visited_set_t *vis) {
    if (depth > max_depth) {
        ios_printf(f, "...");
        return;
    }

    /* 1. fixnum */
    if (isfixnum(v)) {
        int64_t n = numval(v);
        ios_printf(f, "FIXNUM(%ld) [raw=0x%lx]", n, (unsigned long)v);
        return;
    }

    /* 2. 特殊常量 */
    if (v == fl_ctx->NIL) {
        ios_printf(f, "NIL");
        return;
    }
    if (v == UNBOUND) {
        ios_printf(f, "UNBOUND/FWD");
        return;
    }

    uint32_t t = tag(v);

    switch (t) {
        case TAG_CPRIM: {
            cprim_t *cp = (cprim_t*)ptr(v);
            ios_printf(f, "CPRIM@%p", (void*)cp);
            if (cp && cp->type) {
                fltype_t *tp = cp_class(cp);
                ios_printf(f, " [type=");
                if (tp && tp->type) {
                    fl_print(fl_ctx, f, tp->type);
                } else {
                    ios_printf(f, "?");
                }
                ios_printf(f, "]");
            }
            break;
        }

        case TAG_FUNCTION: {
            /* ★★★ 关键：先判断 builtin，避免对立即数解引用 ★★★ */
            if (tinyp(fl_ctx, v)) {
                uint32_t op = uintval(v);
                ios_printf(f, "BUILTIN(op=%u)", (unsigned)op);

                switch (op) {
                    /* 比较与谓词 */
                    case OP_EQ:      ios_printf(f, " [eq]"); break;
                    case OP_EQV:     ios_printf(f, " [eqv]"); break;
                    case OP_EQUAL:   ios_printf(f, " [equal]"); break;
                    case OP_ATOMP:   ios_printf(f, " [atom?]"); break;
                    case OP_NOT:     ios_printf(f, " [not]"); break;
                    case OP_NULLP:   ios_printf(f, " [null?]"); break;
                    case OP_BOOLEANP: ios_printf(f, " [boolean?]"); break;
                    case OP_SYMBOLP:  ios_printf(f, " [symbol?]"); break;
                    case OP_NUMBERP:  ios_printf(f, " [number?]"); break;
                    case OP_BOUNDP:   ios_printf(f, " [bound?]"); break;
                    case OP_PAIRP:    ios_printf(f, " [pair?]"); break;
                    case OP_BUILTINP: ios_printf(f, " [builtin?]"); break;
                    case OP_VECTORP:  ios_printf(f, " [vector?]"); break;
                    case OP_FIXNUMP:  ios_printf(f, " [fixnum?]"); break;
                    case OP_FUNCTIONP: ios_printf(f, " [function?]"); break;

                    /* 列表操作 */
                    case OP_CONS:    ios_printf(f, " [cons]"); break;
                    case OP_LIST:    ios_printf(f, " [list]"); break;
                    case OP_CAR:     ios_printf(f, " [car]"); break;
                    case OP_CDR:     ios_printf(f, " [cdr]"); break;
                    case OP_SETCAR:  ios_printf(f, " [set-car!]"); break;
                    case OP_SETCDR:  ios_printf(f, " [set-cdr!]"); break;
                    case OP_APPLY:   ios_printf(f, " [apply]"); break;

                    /* 算术 */
                    case OP_ADD:     ios_printf(f, " [+]"); break;
                    case OP_SUB:     ios_printf(f, " [-]"); break;
                    case OP_MUL:     ios_printf(f, " [*]"); break;
                    case OP_DIV:     ios_printf(f, " [/]"); break;
                    case OP_IDIV:    ios_printf(f, " [idiv]"); break;
                    case OP_NUMEQ:   ios_printf(f, " [num=]"); break;
                    case OP_LT:      ios_printf(f, " [<]"); break;
                    case OP_COMPARE: ios_printf(f, " [compare]"); break;
                    case OP_ADD2:    ios_printf(f, " [add2]"); break;
                    case OP_SUB2:    ios_printf(f, " [sub2]"); break;
                    case OP_NEG:     ios_printf(f, " [neg]"); break;

                    /* 向量/数组 */
                    case OP_VECTOR:  ios_printf(f, " [vector]"); break;
                    case OP_AREF:    ios_printf(f, " [aref]"); break;
                    case OP_ASET:    ios_printf(f, " [aset]"); break;

                    /* 常量加载 */
                    case OP_LOADT:   ios_printf(f, " [loadt]"); break;
                    case OP_LOADF:   ios_printf(f, " [loadf]"); break;
                    case OP_LOADNIL: ios_printf(f, " [loadnil]"); break;
                    case OP_LOAD0:   ios_printf(f, " [load0]"); break;
                    case OP_LOAD1:   ios_printf(f, " [load1]"); break;
                    case OP_LOADI8:  ios_printf(f, " [loadi8]"); break;

                    /* 变量加载/存储 */
                    case OP_LOADV:   ios_printf(f, " [loadv]"); break;
                    case OP_LOADVL:  ios_printf(f, " [loadvl]"); break;
                    case OP_LOADG:   ios_printf(f, " [loadg]"); break;
                    case OP_LOADGL:  ios_printf(f, " [loadgl]"); break;
                    case OP_LOADA:   ios_printf(f, " [loada]"); break;
                    case OP_LOADAL:  ios_printf(f, " [loadal]"); break;
                    case OP_LOADC:   ios_printf(f, " [loadc]"); break;
                    case OP_LOADCL:  ios_printf(f, " [loadcl]"); break;
                    case OP_SETG:    ios_printf(f, " [setg]"); break;
                    case OP_SETGL:   ios_printf(f, " [setgl]"); break;
                    case OP_SETA:    ios_printf(f, " [seta]"); break;
                    case OP_SETAL:   ios_printf(f, " [setal]"); break;

                    /* 控制流 */
                    case OP_NOP:     ios_printf(f, " [nop]"); break;
                    case OP_DUP:     ios_printf(f, " [dup]"); break;
                    case OP_POP:     ios_printf(f, " [pop]"); break;
                    case OP_JMP:     ios_printf(f, " [jmp]"); break;
                    case OP_BRF:     ios_printf(f, " [brf]"); break;
                    case OP_BRT:     ios_printf(f, " [brt]"); break;
                    case OP_JMPL:    ios_printf(f, " [jmpl]"); break;
                    case OP_BRFL:    ios_printf(f, " [brfl]"); break;
                    case OP_BRTL:    ios_printf(f, " [brtl]"); break;
                    case OP_RET:     ios_printf(f, " [ret]"); break;
                    case OP_CALL:    ios_printf(f, " [call]"); break;
                    case OP_TCALL:   ios_printf(f, " [tcall]"); break;
                    case OP_CALLL:   ios_printf(f, " [calll]"); break;
                    case OP_TCALLL:  ios_printf(f, " [tcalll]"); break;

                    /* 函数/闭包 */
                    case OP_CLOSURE: ios_printf(f, " [closure]"); break;
                    case OP_ARGC:    ios_printf(f, " [argc]"); break;
                    case OP_VARGC:   ios_printf(f, " [vargc]"); break;
                    case OP_LARGC:   ios_printf(f, " [largc]"); break;
                    case OP_LVARGC:  ios_printf(f, " [lvargc]"); break;
                    case OP_TRYCATCH: ios_printf(f, " [trycatch]"); break;
                    case OP_FOR:     ios_printf(f, " [for]"); break;
                    case OP_TAPPLY:  ios_printf(f, " [tapply]"); break;
                    case OP_OPTARGS: ios_printf(f, " [optargs]"); break;
                    case OP_BRBOUND: ios_printf(f, " [brbound]"); break;
                    case OP_KEYARGS: ios_printf(f, " [keyargs]"); break;

                    /* 其他 */
                    case OP_CADR:    ios_printf(f, " [cadr]"); break;
                    case OP_BOX:     ios_printf(f, " [box]"); break;
                    case OP_BOXL:    ios_printf(f, " [boxl]"); break;
                    case OP_SHIFT:   ios_printf(f, " [shift]"); break;
                    case OP_BRNE:    ios_printf(f, " [brne]"); break;
                    case OP_BRNEL:   ios_printf(f, " [brnel]"); break;
                    case OP_BRNN:    ios_printf(f, " [brnn]"); break;
                    case OP_BRNNL:   ios_printf(f, " [brnnl]"); break;
                    case OP_BRN:     ios_printf(f, " [brn]"); break;
                    case OP_BRNL:    ios_printf(f, " [brnl]"); break;
                    case OP_LOADA0:  ios_printf(f, " [loada0]"); break;
                    case OP_LOADA1:  ios_printf(f, " [loada1]"); break;
                    case OP_LOADC0:  ios_printf(f, " [loadc0]"); break;
                    case OP_LOADC1:  ios_printf(f, " [loadc1]"); break;

                    /* 特殊常量 */
                    case OP_BOOL_CONST_T:    ios_printf(f, " [bool#t]"); break;
                    case OP_BOOL_CONST_F:    ios_printf(f, " [bool#f]"); break;
                    case OP_THE_EMPTY_LIST:  ios_printf(f, " [emptylist]"); break;
                    case OP_EOF_OBJECT:      ios_printf(f, " [eof]"); break;

                    default: break;
                }
                break;
            }

            /* 非 builtin：安全的堆指针 */
            function_t *fn = (function_t*)ptr(v);
            if (!fn) {
                ios_printf(f, "FUNC(null)");
                break;
            }
            if (is_visited(vis, fn)) {
                ios_printf(f, "FUNC@%p <cycle>", (void*)fn);
                break;
            }
            mark_visited(vis, fn);

            ios_printf(f, "FUNC@%p [CLOSURE]", (void*)fn);

            ios_printf(f, "\n");
            print_indent(f, depth + 1);
            ios_printf(f, "├─ bcode: ");
            _fl_print_mem_impl(fl_ctx, f, fn->bcode, depth + 1, max_depth, vis);

            ios_printf(f, "\n");
            print_indent(f, depth + 1);
            ios_printf(f, "├─ vals: ");
            _fl_print_mem_impl(fl_ctx, f, fn->vals, depth + 1, max_depth, vis);

            ios_printf(f, "\n");
            print_indent(f, depth + 1);
            ios_printf(f, "├─ env: ");
            _fl_print_mem_impl(fl_ctx, f, fn->env, depth + 1, max_depth, vis);

            ios_printf(f, "\n");
            print_indent(f, depth + 1);
            ios_printf(f, "└─ name: ");
            _fl_print_mem_impl(fl_ctx, f, fn->name, depth + 1, max_depth, vis);
            break;
        }

        case TAG_VECTOR: {
            void *p = ptr(v);
            if (!p) {
                ios_printf(f, "VECTOR(null)");
                break;
            }
            size_t len = vector_size(v);
            ios_printf(f, "VECTOR@%p [len=%zu]", p, len);

            if (len > 0 && depth < max_depth) {
                ios_printf(f, "\n");
                for (size_t i = 0; i < len && i < 8; i++) {
                    print_indent(f, depth + 1);
                    ios_printf(f, "[%zu]: ", i);
                    _fl_print_mem_impl(fl_ctx, f, vector_elt(v, i), depth + 1, max_depth, vis);
                    if (i < len - 1 && i < 7) ios_printf(f, "\n");
                }
                if (len > 8) {
                    ios_printf(f, "\n");
                    print_indent(f, depth + 1);
                    ios_printf(f, "... (%zu more)", len - 8);
                }
            }
            break;
        }

        case TAG_NUM1: {
            ios_printf(f, "NUM1(0x%lx) [raw=0x%lx]",
                       (unsigned long)(v >> 3), (unsigned long)v);
            break;
        }

        case TAG_CVALUE: {
            cvalue_t *cv = (cvalue_t*)ptr(v);
            if (!cv) {
                ios_printf(f, "CVALUE(null)");
                break;
            }
            if (is_visited(vis, cv)) {
                ios_printf(f, "CVALUE@%p <cycle>", (void*)cv);
                break;
            }
            mark_visited(vis, cv);

            fltype_t *tp = cv_class(cv);
            ios_printf(f, "CVALUE@%p", (void*)cv);

            if (tp && tp->type) {
                ios_printf(f, " [type=");
                fl_print(fl_ctx, f, tp->type);
                ios_printf(f, "]");
            }

            ios_printf(f, " [len=%zu]", cv->len);

            if (cv_isstr(fl_ctx, cv) && cv->data) {
                ios_printf(f, " \"%s\"", (char*)cv->data);
            } else if (cv->len > 0 && cv->len <= 16 && cv->data) {
                ios_printf(f, " data=");
                unsigned char *d = (unsigned char*)cv->data;
                for (size_t i = 0; i < cv->len && i < 8; i++) {
                    ios_printf(f, "%02x", d[i]);
                }
            }
            break;
        }

        case TAG_SYM: {
            void *p = ptr(v);
            if (!p) {
                ios_printf(f, "SYM(null)");
                break;
            }

            if (isgensym(fl_ctx, v)) {
                gensym_t *gs = (gensym_t*)p;
                ios_printf(f, "GENSYM#%u", gs ? gs->id : 0);
                if (gs) {
                    ios_printf(f, " [binding=0x%lx]", (unsigned long)gs->binding);
                }
                break;
            }

            symbol_t *sym = (symbol_t*)p;
            const char *name_str = (sym->name[0] != '\0') ? sym->name : "?";

            int is_const = isconstant(sym);
            int is_kw = iskeyword(sym);

            ios_printf(f, "SYM(\"%s\")", name_str);
            ios_printf(f, " [addr=%p", (void*)sym);
            if (is_const) ios_printf(f, ", const");
            if (is_kw) ios_printf(f, ", keyword");
            ios_printf(f, ", binding=0x%lx]", (unsigned long)sym->binding);

            if (sym->binding != UNBOUND && depth < max_depth) {
                ios_printf(f, "\n");
                print_indent(f, depth + 1);
                ios_printf(f, "└─ binding: ");
                _fl_print_mem_impl(fl_ctx, f, sym->binding, depth + 1, max_depth, vis);
            }
            break;
        }

        case TAG_CONS: {
            cons_t *c = (cons_t*)ptr(v);
            if (!c) {
                ios_printf(f, "CONS(null)");
                break;
            }
            if (is_visited(vis, c)) {
                ios_printf(f, "CONS@%p <cycle>", (void*)c);
                break;
            }
            mark_visited(vis, c);

            ios_printf(f, "CONS@%p", (void*)c);

            int car_simple = isfixnum(c->car) || (c->car == fl_ctx->NIL) ||
                             (tag(c->car) == TAG_SYM);
            int cdr_simple = isfixnum(c->cdr) || (c->cdr == fl_ctx->NIL) ||
                             (tag(c->cdr) == TAG_SYM);

            if (car_simple && cdr_simple) {
                ios_printf(f, " { ");
                _fl_print_mem_impl(fl_ctx, f, c->car, depth, max_depth, vis);
                ios_printf(f, " . ");
                _fl_print_mem_impl(fl_ctx, f, c->cdr, depth, max_depth, vis);
                ios_printf(f, " }");
            } else {
                ios_printf(f, "\n");
                print_indent(f, depth + 1);
                ios_printf(f, "├─ car: ");
                _fl_print_mem_impl(fl_ctx, f, c->car, depth + 1, max_depth, vis);
                ios_printf(f, "\n");
                print_indent(f, depth + 1);
                ios_printf(f, "└─ cdr: ");
                _fl_print_mem_impl(fl_ctx, f, c->cdr, depth + 1, max_depth, vis);
            }
            break;
        }

        default: {
            ios_printf(f, "UNKNOWN(tag=%d, raw=0x%lx)", t, (unsigned long)v);
            break;
        }
    }
}
