#include "psdengine.h"

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace psd {
namespace {

  // --------------------------------------------------------------------------
  // パース済みツリー
  // --------------------------------------------------------------------------
  struct Node {
    enum Kind { DICT, ARRAY, STRING, NUMBER, BOOL } kind;
    std::map<std::string, Node*> dict;
    std::vector<std::string>     keyOrder;  // dict のキー出現順 (再直列化用)
    std::vector<Node*>           arr;
    std::string                  str;   // STRING: 生バイト (UTF-16BE, BOM 含む)
    double                       num;
    bool                         isInt; // NUMBER: 整数トークン (小数点なし) だったか
    bool                         bl;

    Node(Kind k) : kind(k), num(0), isInt(false), bl(false) {}
    ~Node() {
      for (std::map<std::string, Node*>::iterator it = dict.begin(); it != dict.end(); ++it)
        delete it->second;
      for (size_t i = 0; i < arr.size(); i++)
        delete arr[i];
    }
  };

  // --------------------------------------------------------------------------
  // トークナイザ兼再帰下降パーサ
  // --------------------------------------------------------------------------
  struct Parser {
    const unsigned char *p;
    const unsigned char *end;
    int depth; // 異常データでの無限再帰を防ぐガード

    Parser(const char *d, size_t n)
      : p((const unsigned char*)d), end((const unsigned char*)d + n), depth(0) {}

    void skipWs() {
      while (p < end) {
        unsigned char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') p++;
        else break;
      }
    }

    static bool isDelim(unsigned char c) {
      return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
             c == '/' || c == '<'  || c == '>'  || c == '['  ||
             c == ']' || c == '('  || c == ')';
    }

    Node *parseValue() {
      skipWs();
      if (p >= end) return 0;
      unsigned char c = *p;
      if (c == '<' && p + 1 < end && p[1] == '<') return parseDict();
      if (c == '[') return parseArray();
      if (c == '(') return parseString();
      return parseToken();
    }

    Node *parseDict() {
      p += 2; // "<<"
      Node *n = new Node(Node::DICT);
      if (++depth > 200) { --depth; return n; }
      while (p < end) {
        skipWs();
        if (p >= end) break;
        if (*p == '>' && p + 1 < end && p[1] == '>') { p += 2; break; }
        if (*p == '/') {
          std::string key = parseName();
          Node *v = parseValue();
          if (v) {
            std::map<std::string, Node*>::iterator it = n->dict.find(key);
            if (it != n->dict.end()) delete it->second;
            else n->keyOrder.push_back(key);
            n->dict[key] = v;
          }
        } else {
          p++; // 想定外バイト: 前進して無限ループ回避
        }
      }
      --depth;
      return n;
    }

    Node *parseArray() {
      p++; // '['
      Node *n = new Node(Node::ARRAY);
      if (++depth > 200) { --depth; return n; }
      while (p < end) {
        skipWs();
        if (p >= end) break;
        if (*p == ']') { p++; break; }
        Node *v = parseValue();
        if (v) n->arr.push_back(v);
        else   p++;
      }
      --depth;
      return n;
    }

    std::string parseName() {
      p++; // '/'
      std::string s;
      while (p < end && !isDelim(*p)) { s.push_back((char)*p); p++; }
      return s;
    }

    // PDF 風の文字列。( ) は入れ子でバランスし、\ は次の 1 バイトをリテラル化。
    // 中身は UTF-16BE の生バイト列としてそのまま保持する。
    Node *parseString() {
      p++; // '('
      Node *n = new Node(Node::STRING);
      int d = 1;
      while (p < end) {
        unsigned char c = *p++;
        if (c == '\\') {
          if (p < end) { n->str.push_back((char)*p); p++; }
          continue;
        }
        if (c == '(') { d++; n->str.push_back('('); continue; }
        if (c == ')') { if (--d == 0) break; n->str.push_back(')'); continue; }
        n->str.push_back((char)c);
      }
      return n;
    }

    Node *parseToken() {
      std::string s;
      while (p < end && !isDelim(*p)) { s.push_back((char)*p); p++; }
      if (s.empty()) { if (p < end) p++; return 0; }
      if (s == "true" || s == "false") {
        Node *n = new Node(Node::BOOL);
        n->bl = (s == "true");
        return n;
      }
      Node *n = new Node(Node::NUMBER);
      n->num = atof(s.c_str());
      // 小数点も指数もなければ整数トークン ("%d" で書く)。
      n->isInt = (s.find('.') == std::string::npos &&
                  s.find('e') == std::string::npos &&
                  s.find('E') == std::string::npos);
      return n;
    }
  };

  // --------------------------------------------------------------------------
  // 抽出ヘルパ
  // --------------------------------------------------------------------------
  Node *dget(Node *n, const char *key) {
    if (!n || n->kind != Node::DICT) return 0;
    std::map<std::string, Node*>::iterator it = n->dict.find(key);
    return it == n->dict.end() ? 0 : it->second;
  }

  // STRING ノードの生バイト (UTF-16BE / BOM 付き) を u16str へ。
  u16str toU16(Node *n) {
    u16str out;
    if (!n || n->kind != Node::STRING) return out;
    const std::string &raw = n->str;
    size_t i = 0;
    if (raw.size() >= 2 &&
        (unsigned char)raw[0] == 0xFE && (unsigned char)raw[1] == 0xFF) {
      i = 2; // BOM
    }
    for (; i + 1 < raw.size(); i += 2) {
      unsigned short hi = (unsigned char)raw[i];
      unsigned short lo = (unsigned char)raw[i + 1];
      out.push_back((char16_t)((hi << 8) | lo));
    }
    return out;
  }

  // --------------------------------------------------------------------------
  // シリアライザ (psd-tools の EngineData 出力形式を厳密に再現)
  //   Dict:  << >> をタブ字下げ + 改行で。 深さ = tab 数。
  //   List:  要素が Dict なら複数行、そうでなければ [ v v ] のインライン。
  //   Float: %.8f を rstrip('0')、末尾 '.' には '0'、|v|<1 は先頭 "0." を "." に。
  //   Int:   %lld、 Bool: true/false、 String: ( BOM + \-escape された UTF16BE )
  // --------------------------------------------------------------------------
  void emitFloat(std::string &o, double v) {
    char b[64];
    snprintf(b, sizeof(b), "%.8f", v);
    std::string s(b);
    size_t e = s.find_last_not_of('0');
    if (e != std::string::npos) s.erase(e + 1);
    if (!s.empty() && s.back() == '.') s.push_back('0');
    if (v > -1.0 && v < 1.0 && v != 0.0) {
      size_t pos = s.find("0.");
      if (pos == 0)                         s.erase(0, 1);   // "0.5" -> ".5"
      else if (pos == 1 && s[0] == '-')     s.erase(1, 1);   // "-0.5" -> "-.5"
    }
    o += s;
  }

  void emitString(std::string &o, const std::string &raw) {
    o.push_back('(');
    for (size_t i = 0; i < raw.size(); i++) {
      unsigned char c = (unsigned char)raw[i];
      if (c == '\\' || c == '(' || c == ')') o.push_back('\\');
      o.push_back((char)c);
    }
    o.push_back(')');
  }

  void emitScalar(std::string &o, Node *n) {
    switch (n->kind) {
    case Node::NUMBER:
      if (n->isInt) { char b[32]; snprintf(b, sizeof(b), "%lld", (long long)n->num); o += b; }
      else          emitFloat(o, n->num);
      break;
    case Node::BOOL:   o += (n->bl ? "true" : "false"); break;
    case Node::STRING: emitString(o, n->str); break;
    default: break;
    }
  }

  void emitValue(std::string &o, Node *n, int indent, bool inlineMode);

  // indent<0 は "None" (インライン) を表す。
  void emitIndent(std::string &o, int indent, char def = ' ') {
    if (indent < 0) { o.push_back(def); return; }
    o.append((size_t)indent, '\t');
  }
  void emitNewline(std::string &o, int indent) {
    if (indent < 0) return;
    o.push_back('\n');
  }

  void emitDict(std::string &o, Node *n, int indent, bool writeContainer) {
    int inner = (indent < 0) ? -1 : indent + 1;
    if (writeContainer) {
      if (indent == 0) emitNewline(o, indent);
      emitNewline(o, indent);
      emitIndent(o, indent);
      o += "<<";
      emitNewline(o, indent);
    }
    for (const std::string &key : n->keyOrder) {
      std::map<std::string, Node*>::iterator it = n->dict.find(key);
      if (it == n->dict.end()) continue;
      Node *value = it->second;
      emitIndent(o, inner);
      o.push_back('/');
      o += key;
      if (value->kind == Node::DICT) {
        emitDict(o, value, inner, true);
      } else {
        o.push_back(' ');
        if (value->kind == Node::ARRAY) {
          bool multiline = !value->arr.empty() && value->arr[0]->kind == Node::DICT;
          emitValue(o, value, multiline ? inner : -1, !multiline);
        } else {
          emitScalar(o, value);
        }
      }
      emitNewline(o, indent);
    }
    if (writeContainer) {
      emitIndent(o, indent);
      o += ">>";
    }
  }

  void emitList(std::string &o, Node *n, int indent) {
    o.push_back('[');
    if (indent < 0) {                      // インライン
      for (size_t i = 0; i < n->arr.size(); i++) {
        Node *item = n->arr[i];
        if (item->kind == Node::DICT) emitDict(o, item, -1, true);
        else { o.push_back(' '); emitScalar(o, item); }
      }
      o.push_back(' ');
    } else {                               // 複数行 (要素は Dict)
      for (size_t i = 0; i < n->arr.size(); i++)
        emitValue(o, n->arr[i], indent, false);
      emitNewline(o, indent);
      emitIndent(o, indent);
    }
    o.push_back(']');
  }

  void emitValue(std::string &o, Node *n, int indent, bool inlineMode) {
    (void)inlineMode;
    switch (n->kind) {
    case Node::DICT:  emitDict(o, n, indent, true); break;
    case Node::ARRAY: emitList(o, n, indent);       break;
    default:          emitScalar(o, n);             break;
    }
  }

} // anonymous namespace

  bool parseEngineData(const char *data, size_t len, TextLayerData &out)
  {
    Parser ps(data, len);
    Node *root = ps.parseValue();
    if (!root || root->kind != Node::DICT) { delete root; return false; }

    bool haveText = false;
    Node *engine = dget(root, "EngineDict");

    // 本文
    if (engine) {
      Node *editor = dget(engine, "Editor");
      Node *text   = dget(editor, "Text");
      if (text && text->kind == Node::STRING) {
        out.text = toU16(text);
        haveText = true;
      }
    }

    // フォント名テーブル (ResourceDict/FontSet[]/Name)
    std::vector<u16str> fonts;
    {
      Node *rd = dget(root, "ResourceDict");
      Node *fs = dget(rd, "FontSet");
      if (fs && fs->kind == Node::ARRAY) {
        for (size_t i = 0; i < fs->arr.size(); i++) {
          fonts.push_back(toU16(dget(fs->arr[i], "Name")));
        }
      }
    }

    // 既定 StyleSheet (ResourceDict/StyleSheetSet[0]/StyleSheetData)。
    // box text の基準 run は FontSize/Font 等の StyleSheetData を持たず、 この既定を
    // 継承する。 これを読まないと FontSize=0 になる (実表示は既定サイズ)。
    float defSize = 0.0f; int defFontIdx = -1; int defTracking = 0;
    float defColor[4] = {0, 0, 0, 1}; bool defHasColor = false;
    {
      Node *rd2 = dget(root, "ResourceDict");
      Node *sss = dget(rd2, "StyleSheetSet");
      Node *dssd = 0;
      if (sss && sss->kind == Node::ARRAY && !sss->arr.empty()) {
        dssd = dget(sss->arr[0], "StyleSheetData");
        if (!dssd) dssd = dget(dget(sss->arr[0], "StyleSheet"), "StyleSheetData");
      }
      if (dssd) {
        Node *f = dget(dssd, "Font");
        if (f && f->kind == Node::NUMBER) defFontIdx = (int)f->num;
        Node *fs = dget(dssd, "FontSize");
        if (fs && fs->kind == Node::NUMBER) defSize = (float)fs->num;
        Node *tk = dget(dssd, "Tracking");
        if (tk && tk->kind == Node::NUMBER) defTracking = (int)tk->num;
        Node *vals = dget(dget(dssd, "FillColor"), "Values");
        if (vals && vals->kind == Node::ARRAY && vals->arr.size() >= 4) {
          defColor[3] = (float)vals->arr[0]->num; // A
          defColor[0] = (float)vals->arr[1]->num; // R
          defColor[1] = (float)vals->arr[2]->num; // G
          defColor[2] = (float)vals->arr[3]->num; // B
          defHasColor = true;
        }
      }
    }

    // ラン単位スタイル (EngineDict/StyleRun)
    if (engine) {
      Node *styleRun = dget(engine, "StyleRun");
      Node *runArray = dget(styleRun, "RunArray");
      Node *runLen   = dget(styleRun, "RunLengthArray");
      if (runArray && runArray->kind == Node::ARRAY) {
        for (size_t i = 0; i < runArray->arr.size(); i++) {
          Node *ssd = dget(dget(runArray->arr[i], "StyleSheet"), "StyleSheetData");
          TextStyleRun r;
          // 既定を先に適用 (未指定フィールドの継承)。
          r.fontSize = defSize;
          r.sizeInherited = true;   // 既定継承 (下で run が上書きすれば false)
          r.tracking = defTracking;
          if (defFontIdx >= 0 && defFontIdx < (int)fonts.size()) r.font = fonts[defFontIdx];
          if (defHasColor) {
            r.color[0] = defColor[0]; r.color[1] = defColor[1];
            r.color[2] = defColor[2]; r.color[3] = defColor[3];
            r.hasColor = true;
          }
          if (runLen && runLen->kind == Node::ARRAY && i < runLen->arr.size() &&
              runLen->arr[i]->kind == Node::NUMBER) {
            r.length = (int)runLen->arr[i]->num;
          }
          if (ssd) {
            Node *fidx = dget(ssd, "Font");
            if (fidx && fidx->kind == Node::NUMBER) {
              int idx = (int)fidx->num;
              if (idx >= 0 && idx < (int)fonts.size()) r.font = fonts[idx];
            }
            Node *fsz = dget(ssd, "FontSize");
            if (fsz && fsz->kind == Node::NUMBER) {
              r.fontSize = (float)fsz->num;   // 明示 = 解決済み px
              r.sizeInherited = false;
            }
            Node *trk = dget(ssd, "Tracking");
            if (trk && trk->kind == Node::NUMBER) r.tracking = (int)trk->num;
            Node *krn = dget(ssd, "Kerning");
            if (krn && krn->kind == Node::NUMBER) r.kerning = (int)krn->num;
            Node *akn = dget(ssd, "AutoKerning");
            if (akn && akn->kind == Node::BOOL) r.autoKerning = akn->bl;
            Node *vals = dget(dget(ssd, "FillColor"), "Values");
            if (vals && vals->kind == Node::ARRAY && vals->arr.size() >= 4) {
              // EngineData の FillColor/Values は [A R G B]。RGBA へ並べ替える。
              r.color[3] = (float)vals->arr[0]->num; // A
              r.color[0] = (float)vals->arr[1]->num; // R
              r.color[1] = (float)vals->arr[2]->num; // G
              r.color[2] = (float)vals->arr[3]->num; // B
              r.hasColor = true;
            }
          }
          out.runs.push_back(r);
        }
      }

      // 段落別の行揃え (ParagraphRun)。 out.justification は先頭段落 (後方互換)、
      // out.paragraphs に全段落分 (length + justification) を格納する。
      Node *paraRun = dget(engine, "ParagraphRun");
      Node *paraArr = dget(paraRun, "RunArray");
      Node *paraLen = dget(paraRun, "RunLengthArray");
      if (paraArr && paraArr->kind == Node::ARRAY && !paraArr->arr.empty()) {
        for (size_t i = 0; i < paraArr->arr.size(); i++) {
          TextParagraph p;
          if (paraLen && paraLen->kind == Node::ARRAY && i < paraLen->arr.size() &&
              paraLen->arr[i]->kind == Node::NUMBER)
            p.length = (int)paraLen->arr[i]->num;
          Node *props = dget(dget(paraArr->arr[i], "ParagraphSheet"), "Properties");
          Node *just  = dget(props, "Justification");
          if (just && just->kind == Node::NUMBER) p.justification = (int)just->num;
          out.paragraphs.push_back(p);
        }
        out.justification = out.paragraphs[0].justification;  // 後方互換
      }
    }

    delete root;
    return haveText;
  }

  // u16str から EngineData 文字列の生バイト (BOM FE FF + UTF-16BE) を作る。
  static std::string buildTextRaw(const u16str &text) {
    std::string s;
    s.push_back((char)0xFE); s.push_back((char)0xFF);   // BOM
    for (size_t i = 0; i < text.size(); i++) {
      char16_t ch = text[i];
      s.push_back((char)((ch >> 8) & 0xff));
      s.push_back((char)(ch & 0xff));
    }
    return s;
  }

  // EngineData をパースしてそのまま再直列化する (バイト一致検証・編集の土台)。
  bool reserializeEngineData(const char *data, size_t len, std::string &out) {
    Parser ps(data, len);
    Node *root = ps.parseValue();
    if (!root || root->kind != Node::DICT) { delete root; return false; }
    out.clear();
    emitDict(out, root, 0, true);
    delete root;
    return true;
  }

  // EngineData の本文 (EngineDict/Editor/Text) を newText に差し替える。 本文長が
  // 変わるので StyleRun / ParagraphRun を「先頭スタイルの単一ラン」に畳んで長さを
  // 合わせる (複数スタイルは失われる)。 末尾に改行 (\r) が無ければ補う。
  bool editEngineDataText(const char *data, size_t len, const u16str &newText,
                          std::string &out) {
    Parser ps(data, len);
    Node *root = ps.parseValue();
    if (!root || root->kind != Node::DICT) { delete root; return false; }

    u16str text = newText;
    if (text.empty() || text[text.size() - 1] != u'\r') text.push_back(u'\r');
    int textLen = (int)text.size();

    Node *engine = dget(root, "EngineDict");
    Node *editor = dget(engine, "Editor");
    Node *tnode  = dget(editor, "Text");
    if (tnode && tnode->kind == Node::STRING) tnode->str = buildTextRaw(text);

    // StyleRun / ParagraphRun を単一ランへ畳む。
    const char *runKeys[2] = { "StyleRun", "ParagraphRun" };
    for (int k = 0; k < 2; k++) {
      Node *run = dget(engine, runKeys[k]);
      if (!run) continue;
      Node *rla = dget(run, "RunLengthArray");
      if (rla && rla->kind == Node::ARRAY) {
        for (size_t i = 0; i < rla->arr.size(); i++) delete rla->arr[i];
        rla->arr.clear();
        Node *num = new Node(Node::NUMBER);
        num->num = textLen; num->isInt = true;
        rla->arr.push_back(num);
      }
      Node *ra = dget(run, "RunArray");
      if (ra && ra->kind == Node::ARRAY && ra->arr.size() > 1) {
        for (size_t i = 1; i < ra->arr.size(); i++) delete ra->arr[i];
        ra->arr.resize(1);
      }
    }

    out.clear();
    emitDict(out, root, 0, true);
    delete root;
    return true;
  }

} // namespace psd
