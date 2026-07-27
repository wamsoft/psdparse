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
    std::vector<Node*>           arr;
    std::string                  str;   // STRING: 生バイト (UTF-16BE, BOM 含む)
    double                       num;
    bool                         bl;

    Node(Kind k) : kind(k), num(0), bl(false) {}
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

    // ラン単位スタイル (EngineDict/StyleRun)
    if (engine) {
      Node *styleRun = dget(engine, "StyleRun");
      Node *runArray = dget(styleRun, "RunArray");
      Node *runLen   = dget(styleRun, "RunLengthArray");
      if (runArray && runArray->kind == Node::ARRAY) {
        for (size_t i = 0; i < runArray->arr.size(); i++) {
          Node *ssd = dget(dget(runArray->arr[i], "StyleSheet"), "StyleSheetData");
          TextStyleRun r;
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
            if (fsz && fsz->kind == Node::NUMBER) r.fontSize = (float)fsz->num;
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

      // 行揃え (先頭段落の Justification)
      Node *paraArr = dget(dget(engine, "ParagraphRun"), "RunArray");
      if (paraArr && paraArr->kind == Node::ARRAY && !paraArr->arr.empty()) {
        Node *props = dget(dget(paraArr->arr[0], "ParagraphSheet"), "Properties");
        Node *just  = dget(props, "Justification");
        if (just && just->kind == Node::NUMBER) out.justification = (int)just->num;
      }
    }

    delete root;
    return haveText;
  }

} // namespace psd
