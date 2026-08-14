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
            Node *fb = dget(ssd, "FauxBold");
            if (fb && fb->kind == Node::BOOL) r.bold = fb->bl;
            Node *fi = dget(ssd, "FauxItalic");
            if (fi && fi->kind == Node::BOOL) r.italic = fi->bl;
            Node *ul = dget(ssd, "Underline");
            if (ul && ul->kind == Node::BOOL) r.underline = ul->bl;
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

  // dict に数値キーを設定 (無ければ末尾に追加)。
  static void setNumberNode(Node *dict, const char *key, double v, bool isInt) {
    std::map<std::string, Node*>::iterator it = dict->dict.find(key);
    Node *n;
    if (it != dict->dict.end() && it->second->kind == Node::NUMBER) {
      n = it->second;
    } else {
      if (it != dict->dict.end()) delete it->second;
      else dict->keyOrder.push_back(key);
      n = new Node(Node::NUMBER);
      dict->dict[key] = n;
    }
    n->num = v; n->isInt = isInt;
  }

  // dict に真偽キーを設定 (無ければ末尾に追加)。
  static void setBoolNode(Node *dict, const char *key, bool v) {
    std::map<std::string, Node*>::iterator it = dict->dict.find(key);
    Node *n;
    if (it != dict->dict.end() && it->second->kind == Node::BOOL) {
      n = it->second;
    } else {
      if (it != dict->dict.end()) delete it->second;
      else dict->keyOrder.push_back(key);
      n = new Node(Node::BOOL);
      dict->dict[key] = n;
    }
    n->bl = v;
  }

  // StyleSheetData に FillColor/Values ([A R G B]) を設定 (無ければ作成)。
  static void setFillColorNode(Node *ssd, const float rgba[4]) {
    Node *fc = dget(ssd, "FillColor");
    if (!fc || fc->kind != Node::DICT) {
      fc = new Node(Node::DICT);
      setNumberNode(fc, "Type", 1, true);   // 1 = RGB
      ssd->dict["FillColor"] = fc;
      ssd->keyOrder.push_back("FillColor");
    }
    Node *vals = dget(fc, "Values");
    if (!vals || vals->kind != Node::ARRAY) {
      if (vals) delete vals; else fc->keyOrder.push_back("Values");
      vals = new Node(Node::ARRAY);
      fc->dict["Values"] = vals;
    }
    for (size_t i = 0; i < vals->arr.size(); i++) delete vals->arr[i];
    vals->arr.clear();
    double argb[4] = { rgba[3], rgba[0], rgba[1], rgba[2] };  // [A R G B]
    for (int i = 0; i < 4; i++) {
      Node *n = new Node(Node::NUMBER);
      n->num = argb[i]; n->isInt = false;
      vals->arr.push_back(n);
    }
  }

  // --------------------------------------------------------------------------
  // リッチテキスト編集の下支え
  // --------------------------------------------------------------------------

  // ツリーの深いコピー。ラン配列を作り直すとき、元のランを雛形として複製する
  // のに使う。書式キーを一から組むと Photoshop が受け付けない組み合わせに
  // なりやすいので、必ず現物を雛形にする。
  static Node *cloneNode(const Node *src) {
    if (!src) return 0;
    Node *n = new Node(src->kind);
    n->num = src->num; n->isInt = src->isInt; n->bl = src->bl; n->str = src->str;
    n->keyOrder = src->keyOrder;
    for (std::map<std::string, Node*>::const_iterator it = src->dict.begin();
         it != src->dict.end(); ++it) {
      n->dict[it->first] = cloneNode(it->second);
    }
    n->arr.reserve(src->arr.size());
    for (size_t i = 0; i < src->arr.size(); i++) n->arr.push_back(cloneNode(src->arr[i]));
    return n;
  }

  // UTF-8 → EngineData の文字列生バイト (BOM FE FF + UTF-16BE)
  static std::string buildTextRawFromUtf8(const std::string &utf8) {
    u16str w = utf8ToU16(utf8);
    std::string s;
    s.push_back((char)0xFE); s.push_back((char)0xFF);
    for (size_t i = 0; i < w.size(); i++) {
      s.push_back((char)((w[i] >> 8) & 0xff));
      s.push_back((char)(w[i] & 0xff));
    }
    return s;
  }

  // ResourceDict/FontSet から名前でインデックスを引く。無ければ既存項目を
  // 雛形に複製して名前だけ差し替えたものを追記し、その位置を返す。
  // FontSet が無い / 空なら -1 (フォント指定は諦める)。
  static int fontIndexFor(Node *root, const std::string &utf8Name) {
    Node *fs = dget(dget(root, "ResourceDict"), "FontSet");
    if (!fs || fs->kind != Node::ARRAY || fs->arr.empty()) return -1;

    const std::string want = buildTextRawFromUtf8(utf8Name);
    for (size_t i = 0; i < fs->arr.size(); i++) {
      Node *nm = dget(fs->arr[i], "Name");
      if (nm && nm->kind == Node::STRING && nm->str == want) return (int)i;
    }
    // 既存項目 (Script / FontType / Synthetic が入っている) を雛形にする
    Node *entry = cloneNode(fs->arr[0]);
    Node *nm = dget(entry, "Name");
    if (!nm || nm->kind != Node::STRING) { delete entry; return -1; }
    nm->str = want;
    fs->arr.push_back(entry);
    return (int)fs->arr.size() - 1;
  }

  // StyleSheetData へ RunStyleEdit を適用する (has* が立っているものだけ)
  static void applyRunStyle(Node *root, Node *ssd, const RunStyleEdit &edit) {
    if (!ssd) return;
    if (edit.hasFont && !edit.font.empty()) {
      int idx = fontIndexFor(root, edit.font);
      if (idx >= 0) setNumberNode(ssd, "Font", (double)idx, true);
    }
    if (edit.hasSize)      setNumberNode(ssd, "FontSize", edit.size, false);
    if (edit.hasTracking)  setNumberNode(ssd, "Tracking", (double)edit.tracking, true);
    if (edit.hasKerning)   setNumberNode(ssd, "Kerning",  (double)edit.kerning,  true);
    if (edit.hasBold)      setBoolNode(ssd, "FauxBold",   edit.bold);
    if (edit.hasItalic)    setBoolNode(ssd, "FauxItalic", edit.italic);
    if (edit.hasUnderline) setBoolNode(ssd, "Underline",  edit.underline);
    if (edit.hasColor)     setFillColorNode(ssd, edit.color);
  }

  // 指定された長さの並びを本文長へ合わせる。長すぎれば切り詰め、短ければ
  // 末尾を伸ばす。空 (または全部 0) なら「全体で 1 つ」にする。
  // kept には採用した元インデックスが入る (スタイル指定の対応付け用)。
  static std::vector<int> fitLengths(const std::vector<int> &want, int textLen,
                              std::vector<size_t> &kept) {
    std::vector<int> out;
    kept.clear();
    int used = 0;
    for (size_t i = 0; i < want.size(); i++) {
      if (used >= textLen) break;
      int l = want[i];
      if (l <= 0) continue;
      if (used + l > textLen) l = textLen - used;
      out.push_back(l);
      kept.push_back(i);
      used += l;
    }
    if (out.empty()) {
      out.push_back(textLen);
      kept.push_back((size_t)-1);      // 雛形をそのまま使う
    } else if (used < textLen) {
      out.back() += textLen - used;    // 端数は末尾へ寄せる
    }
    return out;
  }

  // run の {RunArray, RunLengthArray} を lengths の数だけ作り直す。
  // 既存のランは順に再利用し、足りない分は先頭ランの複製で埋める。
  // apply は各ランに対する追加処理 (スタイル / 行揃えの適用)。
  template <typename ApplyFn>
  void rebuildRun(Node *run, const std::vector<int> &lengths, ApplyFn apply) {
    if (!run) return;
    Node *ra = dget(run, "RunArray");
    if (!ra || ra->kind != Node::ARRAY || ra->arr.empty()) return;

    // 雛形は先に複製しておく。ループ内で ra->arr[0] の所有権を移すので、
    // 元の要素をそのまま雛形として使い回すと 2 個目以降で解放済みを読む。
    Node *tmpl = cloneNode(ra->arr[0]);

    std::vector<Node*> built;
    built.reserve(lengths.size());
    for (size_t i = 0; i < lengths.size(); i++) {
      Node *item;
      if (i < ra->arr.size() && ra->arr[i]) {
        item = ra->arr[i];
        ra->arr[i] = 0;                 // 所有権を built へ移す
      } else {
        item = cloneNode(tmpl);
      }
      apply(item, i);
      built.push_back(item);
    }
    for (size_t i = 0; i < ra->arr.size(); i++) delete ra->arr[i];   // 余り
    ra->arr = built;
    delete tmpl;

    Node *rla = dget(run, "RunLengthArray");
    if (rla && rla->kind == Node::ARRAY) {
      for (size_t i = 0; i < rla->arr.size(); i++) delete rla->arr[i];
      rla->arr.clear();
      for (size_t i = 0; i < lengths.size(); i++) {
        Node *num = new Node(Node::NUMBER);
        num->num = lengths[i]; num->isInt = true;
        rla->arr.push_back(num);
      }
    }
  }

  bool editEngineDataRunStyle(const char *data, size_t len, int runIndex,
                              const RunStyleEdit &edit, std::string &out) {
    Parser ps(data, len);
    Node *root = ps.parseValue();
    if (!root || root->kind != Node::DICT) { delete root; return false; }
    Node *engine   = dget(root, "EngineDict");
    Node *styleRun = dget(engine, "StyleRun");
    Node *runArray = dget(styleRun, "RunArray");
    if (!runArray || runArray->kind != Node::ARRAY ||
        runIndex < 0 || runIndex >= (int)runArray->arr.size()) {
      delete root; return false;
    }
    Node *ssd = dget(dget(runArray->arr[(size_t)runIndex], "StyleSheet"), "StyleSheetData");
    if (!ssd || ssd->kind != Node::DICT) { delete root; return false; }

    applyRunStyle(root, ssd, edit);

    out.clear();
    emitDict(out, root, 0, true);
    delete root;
    return true;
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

  // --------------------------------------------------------------------------
  // リッチテキスト差し替え
  // --------------------------------------------------------------------------
  bool editEngineDataRichText(const char *data, size_t len, const u16str &newText,
                              const std::vector<TextRunSpec> &runs,
                              const std::vector<TextParagraphSpec> &paragraphs,
                              std::string &out) {
    Parser ps(data, len);
    Node *root = ps.parseValue();
    if (!root || root->kind != Node::DICT) { delete root; return false; }

    u16str text = newText;
    if (text.empty() || text[text.size() - 1] != u'\r') text.push_back(u'\r');
    const int textLen = (int)text.size();

    Node *engine = dget(root, "EngineDict");
    Node *tnode  = dget(dget(engine, "Editor"), "Text");
    if (tnode && tnode->kind == Node::STRING) tnode->str = buildTextRaw(text);

    // --- スタイルラン ---
    {
      std::vector<int> want;
      want.reserve(runs.size());
      for (size_t i = 0; i < runs.size(); i++) want.push_back(runs[i].length);
      std::vector<size_t> kept;
      std::vector<int> lengths = fitLengths(want, textLen, kept);

      rebuildRun(dget(engine, "StyleRun"), lengths, [&](Node *item, size_t i) {
        if (i >= kept.size() || kept[i] == (size_t)-1) return;   // 雛形のまま
        Node *ssd = dget(dget(item, "StyleSheet"), "StyleSheetData");
        applyRunStyle(root, ssd, runs[kept[i]].style);
      });
    }

    // --- 段落ラン ---
    {
      std::vector<int> want;
      want.reserve(paragraphs.size());
      for (size_t i = 0; i < paragraphs.size(); i++) want.push_back(paragraphs[i].length);
      std::vector<size_t> kept;
      std::vector<int> lengths = fitLengths(want, textLen, kept);

      rebuildRun(dget(engine, "ParagraphRun"), lengths, [&](Node *item, size_t i) {
        if (i >= kept.size() || kept[i] == (size_t)-1) return;
        const TextParagraphSpec &p = paragraphs[kept[i]];
        if (!p.hasJustification) return;
        Node *props = dget(dget(item, "ParagraphSheet"), "Properties");
        if (props) setNumberNode(props, "Justification", (double)p.justification, true);
      });
    }

    out.clear();
    emitDict(out, root, 0, true);
    delete root;
    return true;
  }

  bool editEngineDataJustification(const char *data, size_t len, int paraIndex,
                                   int justification, std::string &out) {
    Parser ps(data, len);
    Node *root = ps.parseValue();
    if (!root || root->kind != Node::DICT) { delete root; return false; }

    Node *paraArr = dget(dget(dget(root, "EngineDict"), "ParagraphRun"), "RunArray");
    if (!paraArr || paraArr->kind != Node::ARRAY || paraArr->arr.empty()) {
      delete root; return false;
    }
    if (paraIndex >= (int)paraArr->arr.size()) { delete root; return false; }

    bool any = false;
    for (size_t i = 0; i < paraArr->arr.size(); i++) {
      if (paraIndex >= 0 && (size_t)paraIndex != i) continue;
      Node *props = dget(dget(paraArr->arr[i], "ParagraphSheet"), "Properties");
      if (!props) continue;
      setNumberNode(props, "Justification", (double)justification, true);
      any = true;
    }
    if (!any) { delete root; return false; }

    out.clear();
    emitDict(out, root, 0, true);
    delete root;
    return true;
  }

  bool listEngineDataFonts(const char *data, size_t len,
                           std::vector<std::string> &outUtf8Names) {
    outUtf8Names.clear();
    Parser ps(data, len);
    Node *root = ps.parseValue();
    if (!root || root->kind != Node::DICT) { delete root; return false; }

    Node *fs = dget(dget(root, "ResourceDict"), "FontSet");
    if (fs && fs->kind == Node::ARRAY) {
      for (size_t i = 0; i < fs->arr.size(); i++) {
        u16str w = toU16(dget(fs->arr[i], "Name"));
        // u16 → UTF-8 (BMP 外はサロゲートペアを結合)
        std::string s;
        for (size_t k = 0; k < w.size(); k++) {
          unsigned cp = (unsigned)w[k];
          if (cp >= 0xD800 && cp <= 0xDBFF && k + 1 < w.size()) {
            unsigned lo = (unsigned)w[k + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              k++;
            }
          }
          if (cp < 0x80) s += (char)cp;
          else if (cp < 0x800) {
            s += (char)(0xC0 | (cp >> 6));
            s += (char)(0x80 | (cp & 0x3F));
          } else if (cp < 0x10000) {
            s += (char)(0xE0 | (cp >> 12));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
          } else {
            s += (char)(0xF0 | (cp >> 18));
            s += (char)(0x80 | ((cp >> 12) & 0x3F));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
          }
        }
        outUtf8Names.push_back(s);
      }
    }
    delete root;
    return true;
  }

} // namespace psd
