#include "regex_impl.hh"
#include "vector.hh"
#include "unit_tests.hh"
#include "string.hh"
#include "unicode.hh"
#include "utf8.hh"
#include "utf8_iterator.hh"
#include "exception.hh"
#include "array_view.hh"

#include "buffer_utils.hh"

namespace Kakoune
{

struct ParsedRegex
{
    enum Op
    {
        Literal,
        AnyChar,
        Matcher,
        Sequence,
        Alternation,
        LineStart,
        LineEnd,
        WordBoundary,
        NotWordBoundary,
        SubjectBegin,
        SubjectEnd,
    };

    struct Quantifier
    {
        enum Type
        {
            One,
            Optional,
            RepeatZeroOrMore,
            RepeatOneOrMore,
            RepeatMinMax,
        };
        Type type = One;
        int min = -1, max = -1;

        bool allows_none() const
        {
            return type == Quantifier::Optional or
                   type == Quantifier::RepeatZeroOrMore or
                  (type == Quantifier::RepeatMinMax and min <= 0);
        }

        bool allows_infinite_repeat() const
        {
            return type == Quantifier::RepeatZeroOrMore or
                   type == Quantifier::RepeatOneOrMore or
                  (type == Quantifier::RepeatMinMax and max == -1);
        };
    };

    struct AstNode
    {
        Op op;
        Codepoint value;
        Quantifier quantifier;
        Vector<std::unique_ptr<AstNode>> children;
    };

    using AstNodePtr = std::unique_ptr<AstNode>;

    AstNodePtr ast;
    size_t capture_count;
    Vector<std::function<bool (Codepoint)>> matchers;
};

// Recursive descent parser based on naming used in the ECMAScript
// standard, although the syntax is not fully compatible.
struct RegexParser
{
    RegexParser(StringView re)
        : m_regex{re}, m_pos{re.begin(), re}
    {
        m_parsed_regex.capture_count = 1;
        m_parsed_regex.ast = disjunction(0);
    }

    ParsedRegex get_parsed_regex() { return std::move(m_parsed_regex); }

    static ParsedRegex parse(StringView re) { return RegexParser{re}.get_parsed_regex(); }

private:
    struct InvalidPolicy
    {
        Codepoint operator()(Codepoint cp) { throw runtime_error{"Invalid utf8 in regex"}; }
    };

    using Iterator = utf8::iterator<const char*, Codepoint, int, InvalidPolicy>;
    using AstNodePtr = ParsedRegex::AstNodePtr;

    AstNodePtr disjunction(unsigned capture = -1)
    {
        AstNodePtr node = alternative();
        if (at_end() or *m_pos != '|')
        {
            node->value = capture;
            return node;
        }

        ++m_pos;
        AstNodePtr res = new_node(ParsedRegex::Alternation);
        res->children.push_back(std::move(node));
        res->children.push_back(disjunction());
        res->value = capture;
        return res;
    }

    AstNodePtr alternative()
    {
        AstNodePtr res = new_node(ParsedRegex::Sequence);
        while (auto node = term())
            res->children.push_back(std::move(node));
        if (res->children.empty())
            parse_error("empty alternative");
        return res;
    }

    AstNodePtr term()
    {
        if (auto node = assertion())
            return node;
        if (auto node = atom())
        {
            node->quantifier = quantifier();
            return node;
        }
        return nullptr;
    }

    AstNodePtr assertion()
    {
        if (at_end())
            return nullptr;

        switch (*m_pos)
        {
            case '^': ++m_pos; return new_node(ParsedRegex::LineStart);
            case '$': ++m_pos; return new_node(ParsedRegex::LineEnd);
            case '\\':
                if (m_pos+1 == m_regex.end())
                    return nullptr;
                switch (*(m_pos+1))
                {
                    case 'b': m_pos += 2; return new_node(ParsedRegex::WordBoundary);
                    case 'B': m_pos += 2; return new_node(ParsedRegex::NotWordBoundary);
                    case '`': m_pos += 2; return new_node(ParsedRegex::SubjectBegin);
                    case '\'': m_pos += 2; return new_node(ParsedRegex::SubjectEnd);
                }
                break;
            /* TODO: look ahead, look behind */
        }
        return nullptr;
    }

    AstNodePtr atom()
    {
        if (at_end())
            return nullptr;

        const Codepoint cp = *m_pos;
        switch (cp)
        {
            case '.': ++m_pos; return new_node(ParsedRegex::AnyChar);
            case '(':
            {
                ++m_pos;
                auto content = disjunction(m_parsed_regex.capture_count++);

                if (at_end() or *m_pos != ')')
                    parse_error("unclosed parenthesis");
                ++m_pos;
                return content;
            }
            case '\\':
                ++m_pos;
                return atom_escape();
            case '[':
                ++m_pos;
                return character_class();
            default:
                if (contains("^$.*+?()[]{}|", cp))
                    return nullptr;
                ++m_pos;
                return new_node(ParsedRegex::Literal, cp);
        }
    }

    AstNodePtr atom_escape()
    {
        const Codepoint cp = *m_pos++;

        // CharacterClassEscape
        for (auto& character_class : character_class_escapes)
        {
            if (character_class.cp == cp)
            {
                auto matcher_id = m_parsed_regex.matchers.size();
                m_parsed_regex.matchers.push_back(
                    [ctype = wctype(character_class.ctype),
                     chars = character_class.additional_chars] (Codepoint cp) {
                        return iswctype(cp, ctype) or contains(chars, cp);
                    });
                return new_node(ParsedRegex::Matcher, matcher_id);
            }
        }

        // CharacterEscape
        struct { Codepoint name; Codepoint value; } control_escapes[] = {
            { 'f', '\f' }, { 'n', '\n' }, { 'r', '\r' }, { 't', '\t' }, { 'v', '\v' }
        };
        for (auto& control : control_escapes)
        {
            if (control.name == cp)
                return new_node(ParsedRegex::Literal, control.value);
        }

        // TOOD: \c..., \0..., '\0x...', \u...

        if (contains("^$\\.*+?()[]{}|", cp)) // SyntaxCharacter
            return new_node(ParsedRegex::Literal, cp);
        parse_error("unknown atom escape");
    }

    AstNodePtr character_class()
    {
        const bool negative = m_pos != m_regex.end() and *m_pos == '^';
        if (negative)
            ++m_pos;

        struct CharRange { Codepoint min, max; };
        Vector<CharRange> ranges;
        Vector<std::pair<wctype_t, bool>> ctypes;
        while (m_pos != m_regex.end() and *m_pos != ']')
        {
            const auto cp = *m_pos++;
            if (cp == '-')
            {
                ranges.push_back({ '-', '-' });
                continue;
            }

            if (at_end())
                break;

            if (cp == '\\')
            {
                auto it = find_if(character_class_escapes,
                                  [cp = *m_pos](auto& t) { return t.cp == cp; });
                if (it != std::end(character_class_escapes))
                {
                    ctypes.push_back({wctype(it->ctype), not it->neg});
                    for (auto& c : it->additional_chars)
                        ranges.push_back({(Codepoint)c, (Codepoint)c});
                    ++m_pos;
                    continue;
                }
            }

            CharRange range = { cp, cp };
            if (*m_pos == '-')
            {
                if (++m_pos == m_regex.end())
                    break;
                range.max = *m_pos++;
                if (range.min > range.max)
                    parse_error("invalid range specified");
            }
            ranges.push_back(range);
        }
        if (at_end())
            parse_error("unclosed character class");
        ++m_pos;

        auto matcher = [ranges = std::move(ranges),
                        ctypes = std::move(ctypes), negative] (Codepoint cp) {
            auto found = contains_that(ranges, [cp](auto& r) {
                return r.min <= cp and cp <= r.max;
            }) or contains_that(ctypes, [cp](auto& c) {
                return (bool)iswctype(cp, c.first) == c.second;
            });
            return negative ? not found : found;
        };

        auto matcher_id = m_parsed_regex.matchers.size();
        m_parsed_regex.matchers.push_back(std::move(matcher));

        return new_node(ParsedRegex::Matcher, matcher_id);
    }

    ParsedRegex::Quantifier quantifier()
    {
        if (at_end())
            return {ParsedRegex::Quantifier::One};

        auto read_int = [](auto& pos, auto begin, auto end) {
            int res = 0;
            for (; pos != end; ++pos)
            {
                const auto cp = *pos;
                if (cp < '0' or cp > '9')
                    return pos == begin ? -1 : res;
                res = res * 10 + cp - '0';
            }
            return res;
        };

        switch (*m_pos)
        {
            case '*': ++m_pos; return {ParsedRegex::Quantifier::RepeatZeroOrMore};
            case '+': ++m_pos; return {ParsedRegex::Quantifier::RepeatOneOrMore};
            case '?': ++m_pos; return {ParsedRegex::Quantifier::Optional};
            case '{':
            {
                auto it = m_pos+1;
                const int min = read_int(it, it, m_regex.end());
                int max = min;
                if (*it == ',')
                {
                    ++it;
                    max = read_int(it, it, m_regex.end());
                }
                if (*it++ != '}')
                   parse_error("expected closing bracket");
                m_pos = it;
                return {ParsedRegex::Quantifier::RepeatMinMax, min, max};
            }
            default: return {ParsedRegex::Quantifier::One};
        }
    }

    static AstNodePtr new_node(ParsedRegex::Op op, Codepoint value = -1,
                               ParsedRegex::Quantifier quantifier = {ParsedRegex::Quantifier::One})
    {
        return AstNodePtr{new ParsedRegex::AstNode{op, value, quantifier, {}}};
    }


    bool at_end() const { return m_pos == m_regex.end(); }

    [[gnu::noreturn]]
    void parse_error(StringView error)
    {
        throw runtime_error(format("regex parse error: {} at '{}<<<HERE>>>{}'", error,
                                   StringView{m_regex.begin(), m_pos.base()},
                                   StringView{m_pos.base(), m_regex.end()}));
    }

    ParsedRegex m_parsed_regex;
    StringView m_regex;
    Iterator m_pos;

    struct CharacterClassEscape {
        Codepoint cp;
        const char* ctype;
        StringView additional_chars;
        bool neg;
    };

    static const CharacterClassEscape character_class_escapes[6];
};

// For some reason Gcc fails to link if this is constexpr
const RegexParser::CharacterClassEscape RegexParser::character_class_escapes[6] = {
    { 'd', "digit", "", false },
    { 'D', "digit", "", true },
    { 'w', "alnum", "_", false },
    { 'W', "alnum", "_", true },
    { 's', "space", "", false },
    { 's', "space", "", true },
};

struct CompiledRegex
{
    enum Op : char
    {
        Match,
        Literal,
        AnyChar,
        Matcher,
        Jump,
        Split_PrioritizeParent,
        Split_PrioritizeChild,
        Save,
        LineStart,
        LineEnd,
        WordBoundary,
        NotWordBoundary,
        SubjectBegin,
        SubjectEnd,
    };

    using Offset = unsigned;

    Vector<char> bytecode;
    Vector<std::function<bool (Codepoint)>> matchers;
    size_t save_count;
};

struct RegexCompiler
{
    RegexCompiler(const ParsedRegex& parsed_regex)
        : m_parsed_regex{parsed_regex}
    {
        write_search_prefix();
        compile_node(m_parsed_regex.ast);
        push_op(CompiledRegex::Match);
        m_program.matchers = m_parsed_regex.matchers;
        m_program.save_count = m_parsed_regex.capture_count * 2;
    }

    CompiledRegex get_compiled_regex() { return std::move(m_program); }

    using Offset = CompiledRegex::Offset;
    static constexpr Offset search_prefix_size = 3 + 2 * sizeof(Offset);

    static CompiledRegex compile(StringView re)
    {
        return RegexCompiler{RegexParser::parse(re)}.get_compiled_regex();
    }

private:
    Offset compile_node_inner(const ParsedRegex::AstNodePtr& node)
    {
        const auto start_pos = m_program.bytecode.size();

        const Codepoint capture = (node->op == ParsedRegex::Alternation or node->op == ParsedRegex::Sequence) ? node->value : -1;
        if (capture != -1)
        {
            push_op(CompiledRegex::Save);
            push_byte(capture * 2);
        }

        Vector<Offset> goto_inner_end_offsets;
        switch (node->op)
        {
            case ParsedRegex::Literal:
                push_op(CompiledRegex::Literal);
                push_codepoint(node->value);
                break;
            case ParsedRegex::AnyChar:
                push_op(CompiledRegex::AnyChar);
                break;
            case ParsedRegex::Matcher:
                push_op(CompiledRegex::Matcher);
                push_byte(node->value);
            case ParsedRegex::Sequence:
                for (auto& child : node->children)
                    compile_node(child);
                break;
            case ParsedRegex::Alternation:
            {
                auto& children = node->children;
                kak_assert(children.size() == 2);

                push_op(CompiledRegex::Split_PrioritizeParent);
                auto offset = alloc_offset();

                compile_node(children[0]);
                push_op(CompiledRegex::Jump);
                goto_inner_end_offsets.push_back(alloc_offset());

                auto right_pos = compile_node(children[1]);
                get_offset(offset) = right_pos;

                break;
            }
            case ParsedRegex::LineStart:
                push_op(CompiledRegex::LineStart);
                break;
            case ParsedRegex::LineEnd:
                push_op(CompiledRegex::LineEnd);
                break;
            case ParsedRegex::WordBoundary:
                push_op(CompiledRegex::WordBoundary);
                break;
            case ParsedRegex::NotWordBoundary:
                push_op(CompiledRegex::NotWordBoundary);
                break;
            case ParsedRegex::SubjectBegin:
                push_op(CompiledRegex::SubjectBegin);
                break;
            case ParsedRegex::SubjectEnd:
                push_op(CompiledRegex::SubjectEnd);
                break;
        }

        for (auto& offset : goto_inner_end_offsets)
            get_offset(offset) =  m_program.bytecode.size();

        if (capture != -1)
        {
            push_op(CompiledRegex::Save);
            push_byte(capture * 2 + 1);
        }

        return start_pos;
    }

    Offset compile_node(const ParsedRegex::AstNodePtr& node)
    {
        Offset pos = m_program.bytecode.size();
        Vector<Offset> goto_end_offsets;

        if (node->quantifier.allows_none())
        {
            push_op(CompiledRegex::Split_PrioritizeParent);
            goto_end_offsets.push_back(alloc_offset());
        }

        auto inner_pos = compile_node_inner(node);
        // Write the node multiple times when we have a min count quantifier
        for (int i = 1; i < node->quantifier.min; ++i)
            inner_pos = compile_node_inner(node);

        if (node->quantifier.allows_infinite_repeat())
        {
            push_op(CompiledRegex::Split_PrioritizeChild);
            get_offset(alloc_offset()) = inner_pos;
        }
        // Write the node as an optional match for the min -> max counts
        else for (int i = std::max(1, node->quantifier.min); // STILL UGLY !
                  i < node->quantifier.max; ++i)
        {
            push_op(CompiledRegex::Split_PrioritizeParent);
            goto_end_offsets.push_back(alloc_offset());
            compile_node_inner(node);
        }

        for (auto offset : goto_end_offsets)
            get_offset(offset) = m_program.bytecode.size();

        return pos;
    }

    // Add a '.*' as the first instructions for the search use case
    void write_search_prefix()
    {
        kak_assert(m_program.bytecode.empty());
        push_op(CompiledRegex::Split_PrioritizeChild);
        get_offset(alloc_offset()) = search_prefix_size;
        push_op(CompiledRegex::AnyChar);
        push_op(CompiledRegex::Split_PrioritizeParent);
        get_offset(alloc_offset()) = 1 + sizeof(Offset);
    }

    Offset alloc_offset()
    {
        auto pos = m_program.bytecode.size();
        m_program.bytecode.resize(pos + sizeof(Offset));
        return pos;
    }

    Offset& get_offset(Offset pos)
    {
        return *reinterpret_cast<Offset*>(&m_program.bytecode[pos]);
    }

    void push_op(CompiledRegex::Op op)
    {
        m_program.bytecode.push_back(op);
    }

    void push_byte(char byte)
    {
        m_program.bytecode.push_back(byte);
    }

    void push_codepoint(Codepoint cp)
    {
        utf8::dump(std::back_inserter(m_program.bytecode), cp);
    }

    CompiledRegex m_program;
    const ParsedRegex& m_parsed_regex;
};

void dump(const CompiledRegex& program)
{
    for (auto pos = program.bytecode.data(), end = program.bytecode.data() + program.bytecode.size();
         pos < end; )
    {
        printf("%4zd    ", pos - program.bytecode.data());
        const auto op = (CompiledRegex::Op)*pos++;
        switch (op)
        {
            case CompiledRegex::Literal:
                printf("literal %lc\n", utf8::read_codepoint(pos, (const char*)nullptr));
                break;
            case CompiledRegex::AnyChar:
                printf("any char\n");
                break;
            case CompiledRegex::Jump:
                printf("jump %u\n", *reinterpret_cast<const CompiledRegex::Offset*>(&*pos));
                pos += sizeof(CompiledRegex::Offset);
                break;
            case CompiledRegex::Split_PrioritizeParent:
            case CompiledRegex::Split_PrioritizeChild:
            {
                printf("split (prioritize %s) %u\n",
                       op == CompiledRegex::Split_PrioritizeParent ? "parent" : "child",
                       *reinterpret_cast<const CompiledRegex::Offset*>(&*pos));
                pos += sizeof(CompiledRegex::Offset);
                break;
            }
            case CompiledRegex::Save:
                printf("save %d\n", *pos++);
                break;
            case CompiledRegex::Matcher:
                printf("matcher %d\n", *pos++);
                break;
            case CompiledRegex::LineStart:
                printf("line start\n");
                break;
            case CompiledRegex::LineEnd:
                printf("line end\n");
                break;
            case CompiledRegex::WordBoundary:
                printf("word boundary\n");
                break;
            case CompiledRegex::NotWordBoundary:
                printf("not word boundary\n");
                break;
            case CompiledRegex::SubjectBegin:
                printf("subject begin\n");
                break;
            case CompiledRegex::SubjectEnd:
                printf("subject end\n");
                break;
            case CompiledRegex::Match:
                printf("match\n");
        }
    }
}

template<typename Iterator>
struct ThreadedRegexVM
{
    ThreadedRegexVM(const CompiledRegex& program)
      : m_program{program} {}

    struct Thread
    {
        const char* inst;
        Vector<const char*> saves = {};
    };

    enum class StepResult { Consumed, Matched, Failed };
    StepResult step(size_t thread_index)
    {
        const auto prog_start = m_program.bytecode.data();
        const auto prog_end = prog_start + m_program.bytecode.size();
        while (true)
        {
            auto& thread = m_threads[thread_index];
            const Codepoint cp = m_pos == m_end ? 0 : *m_pos;
            const CompiledRegex::Op op = (CompiledRegex::Op)*thread.inst++;
            switch (op)
            {
                case CompiledRegex::Literal:
                    if (utf8::read_codepoint(thread.inst, prog_end) == cp)
                        return StepResult::Consumed;
                    return StepResult::Failed;
                case CompiledRegex::AnyChar:
                    return StepResult::Consumed;
                case CompiledRegex::Jump:
                {
                    auto inst = prog_start + *reinterpret_cast<const CompiledRegex::Offset*>(thread.inst);
                    // if instruction is already going to be executed by another thread, drop this thread
                    if (std::find_if(m_threads.begin(), m_threads.end(),
                                     [inst](const Thread& t) { return t.inst == inst; }) != m_threads.end())
                        return StepResult::Failed;
                    thread.inst = inst;
                    break;
                }
                case CompiledRegex::Split_PrioritizeParent:
                {
                    add_thread(thread_index+1, *reinterpret_cast<const CompiledRegex::Offset*>(thread.inst), thread.saves);
                    // thread is invalidated now, as we mutated the m_thread vector
                    m_threads[thread_index].inst += sizeof(CompiledRegex::Offset);
                    break;
                }
                case CompiledRegex::Split_PrioritizeChild:
                {
                    add_thread(thread_index+1, thread.inst + sizeof(CompiledRegex::Offset) - prog_start, thread.saves);
                    // thread is invalidated now, as we mutated the m_thread vector
                    m_threads[thread_index].inst = prog_start + *reinterpret_cast<const CompiledRegex::Offset*>(m_threads[thread_index].inst);
                    break;
                }
                case CompiledRegex::Save:
                {
                    const char index = *thread.inst++;
                    thread.saves[index] = m_pos.base();
                    break;
                }
                case CompiledRegex::Matcher:
                {
                    const int matcher_id = *thread.inst++;
                    return m_program.matchers[matcher_id](*m_pos) ?
                        StepResult::Consumed : StepResult::Failed;
                }
                case CompiledRegex::LineStart:
                    if (not is_line_start())
                        return StepResult::Failed;
                    break;
                case CompiledRegex::LineEnd:
                    if (not is_line_end())
                        return StepResult::Failed;
                    break;
                case CompiledRegex::WordBoundary:
                    if (not is_word_boundary())
                        return StepResult::Failed;
                    break;
                case CompiledRegex::NotWordBoundary:
                    if (is_word_boundary())
                        return StepResult::Failed;
                    break;
                case CompiledRegex::SubjectBegin:
                    if (m_pos != m_begin)
                        return StepResult::Failed;
                    break;
                case CompiledRegex::SubjectEnd:
                    if (m_pos != m_end)
                        return StepResult::Failed;
                    break;
                case CompiledRegex::Match:
                    thread.inst = nullptr;
                    return StepResult::Matched;
            }
        }
        return StepResult::Failed;
    }

    bool exec(StringView data, bool match = true, bool longest = false)
    {
        bool found_match = false;
        m_threads.clear();
        add_thread(0, match ? RegexCompiler::search_prefix_size : 0,
                   Vector<const char*>(m_program.save_count, nullptr));

        m_begin = data.begin();
        m_end = data.end();

        for (m_pos = Utf8It{m_begin, m_begin, m_end}; m_pos != m_end; ++m_pos)
        {
            for (int i = 0; i < m_threads.size(); ++i)
            {
                const auto res = step(i);
                if (res == StepResult::Matched)
                {
                    if (match)
                        continue; // We are not at end, this is not a full match

                    m_captures = std::move(m_threads[i].saves);
                    found_match = true;
                    m_threads.resize(i); // remove this and lower priority threads
                    if (not longest)
                        return true;
                }
                else if (res == StepResult::Failed)
                    m_threads[i].inst = nullptr;
            }
            m_threads.erase(std::remove_if(m_threads.begin(), m_threads.end(),
                                           [](const Thread& t) { return t.inst == nullptr; }), m_threads.end());
            if (m_threads.empty())
                return false;
        }

        // Step remaining threads to see if they match without consuming anything else
        for (int i = 0; i < m_threads.size(); ++i)
        {
            if (step(i) == StepResult::Matched)
            {
                m_captures = std::move(m_threads[i].saves);
                found_match = true;
                m_threads.resize(i); // remove this and lower priority threads
                if (not longest)
                    return true;
            }
        }
        return found_match;
    }

    void add_thread(int index, CompiledRegex::Offset pos, Vector<const char*> saves)
    {
        const char* inst = m_program.bytecode.data() + pos;
        if (std::find_if(m_threads.begin(), m_threads.end(),
                         [inst](const Thread& t) { return t.inst == inst; }) == m_threads.end())
            m_threads.insert(m_threads.begin() + index, {inst, std::move(saves)});
    }

    bool is_line_start() const
    {
        return m_pos == m_begin or *(m_pos-1) == '\n';
    }

    bool is_line_end() const
    {
        return m_pos == m_end or *m_pos == '\n';
    }

    bool is_word_boundary() const
    {
        return m_pos == m_begin or m_pos == m_end or
               is_word(*(m_pos-1)) != is_word(*m_pos);
    }

    const CompiledRegex& m_program;
    Vector<Thread> m_threads;

    using Utf8It = utf8::iterator<Iterator>;

    Iterator m_begin;
    Iterator m_end;
    Utf8It m_pos;

    Vector<const char*> m_captures;
};

void validate_regex(StringView re)
{
    try
    {
        RegexParser{re};
    }
    catch (runtime_error& err)
    {
        write_to_debug_buffer(err.what());
    }
}

auto test_regex = UnitTest{[]{
    {
        auto program = RegexCompiler::compile(R"(a*b)");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("b"));
        kak_assert(vm.exec("ab"));
        kak_assert(vm.exec("aaab"));
        kak_assert(not vm.exec("acb"));
        kak_assert(not vm.exec("abc"));
        kak_assert(not vm.exec(""));
    }

    {
        auto program = RegexCompiler::compile(R"(^a.*b$)");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("afoob"));
        kak_assert(vm.exec("ab"));
        kak_assert(not vm.exec("bab"));
        kak_assert(not vm.exec(""));
    }

    {
        auto program = RegexCompiler::compile(R"(^(foo|qux|baz)+(bar)?baz$)");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("fooquxbarbaz"));
        kak_assert(StringView{vm.m_captures[2], vm.m_captures[3]} == "qux");
        kak_assert(not vm.exec("fooquxbarbaze"));
        kak_assert(not vm.exec("quxbar"));
        kak_assert(not vm.exec("blahblah"));
        kak_assert(vm.exec("bazbaz"));
        kak_assert(vm.exec("quxbaz"));
    }

    {
        auto program = RegexCompiler::compile(R"(.*\b(foo|bar)\b.*)");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("qux foo baz"));
        kak_assert(StringView{vm.m_captures[2], vm.m_captures[3]} == "foo");
        kak_assert(not vm.exec("quxfoobaz"));
        kak_assert(vm.exec("bar"));
        kak_assert(not vm.exec("foobar"));
    }
    {
        auto program = RegexCompiler::compile(R"((foo|bar))");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("foo"));
        kak_assert(vm.exec("bar"));
        kak_assert(not vm.exec("foobar"));
    }

    {
        auto program = RegexCompiler::compile(R"(a{3,5}b)");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(not vm.exec("aab"));
        kak_assert(vm.exec("aaab"));
        kak_assert(not vm.exec("aaaaaab"));
        kak_assert(vm.exec("aaaaab"));
    }

    {
        auto program = RegexCompiler::compile(R"(a{3}b)");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(not vm.exec("aab"));
        kak_assert(vm.exec("aaab"));
        kak_assert(not vm.exec("aaaab"));
    }

    {
        auto program = RegexCompiler::compile(R"(a{3,}b)");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(not vm.exec("aab"));
        kak_assert(vm.exec("aaab"));
        kak_assert(vm.exec("aaaaab"));
    }

    {
        auto program = RegexCompiler::compile(R"(a{,3}b)");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("b"));
        kak_assert(vm.exec("ab"));
        kak_assert(vm.exec("aaab"));
        kak_assert(not vm.exec("aaaab"));
    }

    {
        auto program = RegexCompiler::compile(R"(f.*a(.*o))");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("blahfoobarfoobaz", false, true));
        kak_assert(StringView{vm.m_captures[0], vm.m_captures[1]} == "foobarfoo");
        kak_assert(StringView{vm.m_captures[2], vm.m_captures[3]} == "rfoo");
        kak_assert(vm.exec("mais que fais la police", false, true));
        kak_assert(StringView{vm.m_captures[0], vm.m_captures[1]} == "fais la po");
        kak_assert(StringView{vm.m_captures[2], vm.m_captures[3]} == " po");
    }

    {
        auto program = RegexCompiler::compile(R"([àb-dX-Z]{3,5})");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("càY"));
        kak_assert(not vm.exec("àeY"));
        kak_assert(vm.exec("dcbàX"));
        kak_assert(not vm.exec("efg"));
    }

    {
        auto program = RegexCompiler::compile(R"(\d{3})");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("123"));
        kak_assert(not vm.exec("1x3"));
    }

    {
        auto program = RegexCompiler::compile(R"([-\d]+)");
        dump(program);
        ThreadedRegexVM<const char*> vm{program};
        kak_assert(vm.exec("123-456"));
        kak_assert(not vm.exec("123_456"));
    }
}};

}