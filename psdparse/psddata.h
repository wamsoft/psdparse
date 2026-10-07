#ifndef __psddata_h__
#define __psddata_h__

#include "psdbase.h"
#include "psddesc.h"

#include <vector>
#include <map>
#include <memory>
#include <string>
#include <cstring>

namespace psd {
  // レイヤタイプ
  enum LayerType {
    LAYER_TYPE_NORMAL,    // 通常レイヤ
    LAYER_TYPE_HIDDEN,    // フォルダ区切り
    LAYER_TYPE_FOLDER,    // フォルダレイヤ
    LAYER_TYPE_ADJUST,    // 調整レイヤ
    LAYER_TYPE_FILL,      // 塗りつぶしレイヤ
    LAYER_TYPE_TEXT,      // テキストレイヤ
  };

  // 単位
  enum Unit {
    UNIT_INCH    = 1,    // inch
    UNIT_CM      = 2,    // cm
    UNIT_POINT   = 3,    // points
    UNIT_PICA    = 4,    // pica
    UNIT_COLUMN  = 5,    // columns
  };

  enum ChannelId {
    CH_ID_REAL_UMASK = -3,
    CH_ID_UMASK      = -2,
    CH_ID_TRANSP     = -1,
    CH_ID_GRAY = 0,
    CH_ID_RGB_R = 0,
    CH_ID_RGB_G = 1,
    CH_ID_RGB_B = 2,
    CH_ID_CMYK_C = 0,
    CH_ID_CMYK_M = 1,
    CH_ID_CMYK_Y = 2,
    CH_ID_CMYK_K = 3,
  };

  enum ColorMode {
    COLOR_MODE_BITMAP        = 0,
    COLOR_MODE_GRAYSCALE     = 1,
    COLOR_MODE_INDEXED       = 2,
    COLOR_MODE_RGB           = 3,
    COLOR_MODE_CMYK          = 4,
    COLOR_MODE_MULTICHANNEL  = 7,
    COLOR_MODE_DUOTONE       = 8,
    COLOR_MODE_LAB           = 9,
  };

  enum ColorSpace {
    COLOR_SPACE_DUMMY = -1,
    COLOR_SPACE_RGB,
    COLOR_SPACE_HSB,
    COLOR_SPACE_CMYK,
    COLOR_SPACE_PANTONE,
    COLOR_SPACE_FOCOLTONE,
    COLOR_SPACE_TRUMATCH,
    COLOR_SPACE_TOYO,
    COLOR_SPACE_LAB,
    COLOR_SPACE_GRAY,
    COLOR_SPACE_WIDECMYK,
    COLOR_SPACE_HKS,
    COLOR_SPACE_DIC,
    COLOR_SPACE_TOTALINK,
    COLOR_SPACE_MONITORRGB,
    COLOR_SPACE_DUOTONE,
    COLOR_SPACE_OPACITY,
    COLOR_SPACE_WEB,
    COLOR_SPACE_GRAYFLOAT,
    COLOR_SPACE_RGBFLOAT,
    COLOR_SPACE_OPACITYFLOAT,
  };

  enum BlendMode {
    BLEND_MODE_INVALID = -1,  // invalid(unsupported mode)
    BLEND_MODE_NORMAL,        // normal
    BLEND_MODE_DISSOLVE,      // dissolve
    BLEND_MODE_DARKEN,        // darken
    BLEND_MODE_MULTIPLY,      // multiply
    BLEND_MODE_COLOR_BURN,    // color burn
    BLEND_MODE_LINEAR_BURN,   // linear burn
    BLEND_MODE_LIGHTEN,       // lighten
    BLEND_MODE_SCREEN,        // screen
    BLEND_MODE_COLOR_DODGE,   // color dodge
    BLEND_MODE_LINEAR_DODGE,  // linear dodge
    BLEND_MODE_OVERLAY,       // overlay
    BLEND_MODE_SOFT_LIGHT,    // soft light
    BLEND_MODE_HARD_LIGHT,    // hard light
    BLEND_MODE_VIVID_LIGHT,   // vivid light
    BLEND_MODE_LINEAR_LIGHT,  // linear light
    BLEND_MODE_PIN_LIGHT,     // pin light
    BLEND_MODE_HARD_MIX,      // hard mix
    BLEND_MODE_DIFFERENCE,    // difference
    BLEND_MODE_EXCLUSION,     // exclusion
    BLEND_MODE_HUE,           // hue
    BLEND_MODE_SATURATION,    // saturation
    BLEND_MODE_COLOR,         // color
    BLEND_MODE_LUMINOSITY,    // luminosity
    BLEND_MODE_PASS_THROUGH,  // pass
    // 以降は libpsd 非互換
    BLEND_MODE_DARKER_COLOR,  // darker color
    BLEND_MODE_LIGHTER_COLOR, // lighter color
    BLEND_MODE_SUBTRACT,      // subtract
    BLEND_MODE_DIVIDE,        // divide
  };

  inline BlendMode blendKeyToMode(int blendModeKey) {
    switch (blendModeKey) {
    case 'norm': return BLEND_MODE_NORMAL;
    case 'diss': return BLEND_MODE_DISSOLVE;
    case 'dark': return BLEND_MODE_DARKEN;
    case 'mul ': return BLEND_MODE_MULTIPLY;
    case 'idiv': return BLEND_MODE_COLOR_BURN;
    case 'lbrn': return BLEND_MODE_LINEAR_BURN;
    case 'dkCl': return BLEND_MODE_DARKER_COLOR;
    case 'lite': return BLEND_MODE_LIGHTEN;
    case 'scrn': return BLEND_MODE_SCREEN;
    case 'div ': return BLEND_MODE_COLOR_DODGE;
    case 'lddg': return BLEND_MODE_LINEAR_DODGE;
    // Photoshop が書くキーは 'lgCl' (psd_tools / 仕様書とも一致)。
    // 'ltCl' は本実装が長らく持っていた誤りで、読み側だけ別名として残す。
    case 'lgCl': return BLEND_MODE_LIGHTER_COLOR;
    case 'ltCl': return BLEND_MODE_LIGHTER_COLOR;
    case 'over': return BLEND_MODE_OVERLAY;
    case 'sLit': return BLEND_MODE_SOFT_LIGHT;
    case 'hLit': return BLEND_MODE_HARD_LIGHT;
    case 'vLit': return BLEND_MODE_VIVID_LIGHT;
    case 'lLit': return BLEND_MODE_LINEAR_LIGHT;
    case 'pLit': return BLEND_MODE_PIN_LIGHT;
    case 'hMix': return BLEND_MODE_HARD_MIX;
    case 'diff': return BLEND_MODE_DIFFERENCE;
    case 'smud': return BLEND_MODE_EXCLUSION;
    case 'fsub': return BLEND_MODE_SUBTRACT;
    case 'fdiv': return BLEND_MODE_DIVIDE;
    case 'hue ': return BLEND_MODE_HUE;
    case 'sat ': return BLEND_MODE_SATURATION;
    case 'colr': return BLEND_MODE_COLOR;
    case 'lum ': return BLEND_MODE_LUMINOSITY;
    case 'pass': return BLEND_MODE_PASS_THROUGH;
    default:     return BLEND_MODE_INVALID;
    }
  }

  // ガイドデータの方向
  enum GuideDirection {
    GUIDE_DIR_VERTICAL = 0,
    GGUIDE_DIR_HORIZONTAL = 1
  };

	// ヘッダ情報
	struct Header {
		int version;
		int channels;
		int height;
		int width;
		int depth;
		int mode;
		double hres = 72.0;   // 水平解像度 dpi (image resource 1005)。 既定 72。
		double vres = 72.0;   // 垂直解像度 dpi。

		// PSB (large document format, version 2) か。PSB ではセクション長・
		// チャンネル長・一部の追加情報の長さが 8 バイトになり、RLE の行バイト数
		// テーブルも 1 行 4 バイトになる。
		bool isPSB() const { return version == 2; }
	};

	// PSB で長さフィールドが 8 バイトになる追加情報キー。仕様書が挙げる 13 種に、
	// 仕様書には無いが Photoshop が 8 バイトで書くもの (psd-tools も同じ扱い)
	// を加えたもの。PSD (version 1) ではどれも 4 バイト。
	inline bool isLongLengthKey(int key) {
		switch (key) {
		case 'LMsk': case 'Lr16': case 'Lr32': case 'Layr': case 'Mt16':
		case 'Mt32': case 'Mtrn': case 'Alph': case 'FMsk': case 'lnk2':
		case 'FEid': case 'FXid': case 'PxSD':
		case 'lnk3': case 'lnkE': case 'pths': case 'extd': case 'extn':
		case 'FELS': case 'cinf': case 'artd':
			return true;
		default:
			return false;
		}
	}

	// レイヤ&マスク情報の末尾 (global layer mask info の後ろ) に並ぶ、文書
	// ぜんたいの追加情報ブロック 1 件の位置。offset 類は layerAndMaskTrailing
	// 先頭からのバイト位置。中身は読まずに位置だけ覚え、書き出しでは元の
	// 範囲をそのまま転送する (巨大な lnk2 をメモリに載せないため)。
	struct GlobalBlockInfo {
		int sigType = 0;      // 0: '8BIM' / 1: '8B64'
		int key = 0;
		int offset = 0;       // ブロック先頭 (シグネチャ) の位置
		int dataOffset = 0;   // 中身の先頭
		int dataLength = 0;   // 中身の長さ (長さフィールドの値)
		int padding = 0;      // 中身の後ろの詰め物バイト数
		int total() const { return dataOffset - offset + dataLength + padding; }
	};

	// --- パス (ベクタマスク / 保存パス / 作業パス) -----------------------------
	//
	// パスは 26 バイトのレコード (2 バイトの種別 + 24 バイト) の並び。座標は
	// 符号付き 8.24 固定小数で、文書の幅 / 高さに対する比率 (0..1) として持つ。
	// ディスク上は (縦, 横) の順だが、ここでは x / y に直して持つ。

	// パス上の 1 点 (文書幅 / 高さに対する比率)
	struct PathPoint {
		double x = 0.0;
		double y = 0.0;
	};

	// ベジェのアンカー 1 個と、その前後の制御点
	struct PathKnot {
		bool linked = true;      // 制御点が連動しているか
		PathPoint preceding;     // 前の区間の制御点 (アンカーへ入ってくる側)
		PathPoint anchor;
		PathPoint leaving;       // 次の区間の制御点 (アンカーから出ていく側)
	};

	// サブパス 1 本
	struct PathSubpath {
		bool closed = true;
		// サブパスどうしの合成方法 (長さレコードの 3〜4 バイト目)。
		//   -1 / 1: 結合 (or)、2: 前面の型抜き (subtract)、3: 交差 (intersect)、
		//   0: 中マド (xor)。古いファイルは -1。
		int operation = -1;
		int index = 0;           // 長さレコードの index (Photoshop がシェイプ内の順に振る)
		std::vector<PathKnot> knots;
	};

	struct PathData {
		std::vector<PathSubpath> subpaths;
		bool hasFillRule = false;     // パス塗りつぶしルールのレコード (中身は無い)
		int  initialFill = -1;        // 初期塗りつぶしルール (0/1)。レコード無しは -1
		bool hasClipboard = false;    // クリップボードのレコード
		double clipboardTop = 0, clipboardLeft = 0, clipboardBottom = 0, clipboardRight = 0;
		double clipboardResolution = 0;
	};

	// レイヤのベクタマスク ('vmsk' / シェイプレイヤは 'vsms')
	struct VectorMask {
		bool present = false;
		int key = 0;              // 'vmsk' / 'vsms'
		int version = 0;          // 3
		uint32_t flags = 0;       // bit0: 反転 / bit1: リンク解除 / bit2: 無効
		PathData path;
		bool inverted() const { return (flags & 1) != 0; }
		bool notLinked() const { return (flags & 2) != 0; }
		bool disabled() const { return (flags & 4) != 0; }
	};

	// 保存パス (image resource 2000〜2997) と作業パス (1025)
	struct SavedPath {
		int id = 0;
		std::string name;         // リソース名 (Pascal 文字列の生バイト)
		u16str nameUnicode;       // 'pths' ブロックにある Unicode 名
		bool hasNameUnicode = false;
		PathData path;
	};

	// --- アルファ / スポットチャンネル ---------------------------------------
	// 合成画像の色チャンネルの後ろに続く余分なチャンネル 1 本 (1006 / 1045 / 1077)
	struct AlphaChannelInfo {
		int plane = 0;             // 合成画像のチャンネル番号 (PSDFile::getMergedChannel に渡す)
		u16str name;               // 1045 (Unicode)、無ければ 1006 の Pascal 名を ASCII として
		std::string nameRaw;       // 1006 の Pascal 名の生バイト (1045 から取ったときは空)
		bool hasDisplay = false;   // 1077 (DisplayInfo) があったか
		int colorSpace = 0;        // 表示色の色空間 (0 RGB / 1 HSB / 2 CMYK / 7 Lab / 8 Gray)
		int color[4] = {0, 0, 0, 0};
		int opacity = 0;           // 0..100
		int kind = 0;              // 0 選択範囲を色で表示 / 1 マスク範囲を色で表示 / 2 スポット
	};
	// 画像モードの色チャンネル数 (Multichannel は 0)
	int colorChannelCount(int mode);

	// --- パターン ('Patt' / 'Pat2' / 'Pat3') -----------------------------------
	// 一覧は位置だけを持ち、画素は PSDFile::getPatternImage で展開する。
	struct PatternInfo {
		int blockKey = 0;          // 'Patt' / 'Pat2' / 'Pat3'
		u16str name;
		std::string id;            // パターンの ID (塗りつぶしレイヤ等が参照する)
		int mode = 0;              // 画像モード (COLOR_MODE_*)
		int width = 0, height = 0;
		std::vector<uint8_t> palette;   // Indexed のとき 256 x RGB
		int offset = 0;            // パターン本体の位置 (layerAndMaskTrailing 上)
		int length = 0;
	};

	// --- スマートオブジェクト -------------------------------------------------

	// 文書末尾の lnk2 / lnk3 / lnkD / lnkE にある 1 件 (埋め込み / 外部 / エイリアス)。
	// 中身は位置だけ持ち、取り出しは PSDFile::getLinkedFileData で。
	struct LinkedFileInfo {
		int blockKey = 0;          // 'lnk2' など
		std::string kind;          // "liFD" 埋め込み / "liFE" 外部 / "liFA" エイリアス
		int version = 0;
		std::string uuid;          // レイヤの SoLd の Idnt と対応する
		u16str fileName;
		std::string fileType;      // 4 文字 ("png " / "8BPS" など)
		std::string creator;
		uint64_t dataSize = 0;
		bool hasData = false;      // 中身を持っているか (埋め込み、または外部の写し)
		int dataOffset = 0;        // 中身の位置 (layerAndMaskTrailing 上)
	};

	// レイヤ側のスマートオブジェクト情報 (SoLd / SoLE、旧形式の PlLd)
	struct SmartObjectInfo {
		bool present = false;
		int key = 0;               // 'SoLd' / 'SoLE' / 'PlLd'
		std::string uuid;          // 中身のファイル (LinkedFileInfo::uuid)
		std::string placedId;      // このインスタンスの ID (スマートフィルタのキャッシュの鍵)
		int page = 0, totalPages = 0, antiAlias = 0;
		int placedType = 0;        // 0 不明 / 1 ベクタ / 2 ラスタ / 3 画像スタック
		bool hasTransform = false;
		double transform[8] = {0}; // 四隅 (左上, 右上, 右下, 左下) の x, y
		bool hasSize = false;
		double width = 0, height = 0;   // 中身の元の大きさ (px)
		bool hasFilters = false;   // スマートフィルタが掛かっている
		bool filtersEnabled = false;
	};

	// --- 調整レイヤ -------------------------------------------------------------
	//
	// 調整の種類ごとに形がばらばらなので、名前付きの数値 / 配列 / 表 (行の並び) /
	// 文字列の入れ物に入れる。descriptor 形式の調整 (vibA / blwh / clrL) と、
	// 明るさ・コントラストの新しい値 (CgEd) は descriptor で持つ。
	// 各項目の意味は docs/PYTHON_API.md の Adjustment layers を参照。
	struct AdjustmentInfo {
		int key = 0;             // 'levl' など
		std::string type;        // "levels" / "curves" / ...
		bool valid = true;       // バイナリ形式が最後まで読めたか
		std::vector<std::pair<std::string, double>> scalars;
		std::vector<std::pair<std::string, std::vector<double>>> arrays;
		std::vector<std::pair<std::string, std::vector<std::vector<double>>>> tables;
		std::vector<std::pair<std::string, std::string>> text;
		std::vector<std::pair<std::string, u16str>> unicode;
		std::shared_ptr<Descriptor> descriptor;
	};

	// パスレコード列 (26 バイト × n) を読む。読めたところまでを out に入れ、
	// 途中で壊れていたら false。
	bool parsePathRecords(const uint8_t *p, size_t n, PathData &out);

	struct LayerInfo;
	// 調整レイヤのパラメータを読む。調整のブロックが無ければ false。
	bool decodeAdjustment(const LayerInfo &layer, AdjustmentInfo &out);
	// 旧形式のレイヤー効果 'lrFX' を効果ごとに読む (AdjustmentInfo を名前付きの値の
	// 入れ物として使う。type は "drop_shadow" など)。'lrFX' が無ければ false。
	bool decodeLegacyEffects(const LayerInfo &layer, std::vector<AdjustmentInfo> &out);

  // RGBAカラー
  struct ColorRgba {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
  };

  // カラーテーブル
  struct ColorTable {
    ColorTable() : transparencyIndex(-1), validCount(0) {}
    std::vector<ColorRgba> colors; // 参照側の便宜上常にフルサイズ(256)保証
    int16_t transparencyIndex;     // 透明色インデックス(当該エントリはa=0x0セット済)
    int16_t validCount;            // 有効なエントリ数
  };

  // レイヤーカンプ
  struct LayerComp {
    int id;
    bool isRecordVisibility;
    bool isRecordPosition;
    bool isRecordAppearance;
    u16str name;
    u16str comment;
  };

  // レイヤーごとのレイヤーカンプ情報
  struct LayerCompInfo {
    int id;
    int offsetX;
    int offsetY;
    bool isEnabled;
  };

  // スライスアイテム
  struct SliceItem {
    int id;
    int groupId;
    int origin;
    int associatedLayerId; // Only present if Origin = 1
    u16str name;
    int type;
    int left;
    int top;
    int right;
    int bottom;
    u16str url;
    u16str target;
    u16str message;
    u16str altTag;
    bool isCellTextHtml;
    u16str cellText;
    int horizontalAlign;
    int verticalAlign;
    uint8_t colorA;
    uint8_t colorR;
    uint8_t colorG;
    uint8_t colorB;
  };

  // スライスリソース
  struct SliceResource {
    SliceResource() : isEnabled(false) {}

    bool isEnabled;
    int boundingLeft;
    int boundingTop;
    int boundingRight;
    int boundingBottom;
    u16str groupName;
    std::vector<SliceItem> slices;
  };

  // ガイドアイテム
  struct GuideItem {
    int location;
    GuideDirection direction;
  };
  
  // グリッドガイドリソース
  struct GridGuideResource {
    GridGuideResource() : isEnabled(false) {}
    
    bool isEnabled;
    int horizontalGrid;
    int verticalGrid;
    std::vector<GuideItem> guides;
  };

	// イメージリソース情報
	struct ImageResourceInfo {
		ImageResourceInfo(uint16_t id, std::string &name, int size, IteratorBase *data) : id(id), name(name), size(size), data(data) {};
		~ImageResourceInfo() {
			delete data;
		}
		ImageResourceInfo(const ImageResourceInfo &self) {
			this->id   = self.id;
			this->name = self.name;
			this->size = self.size;
			this->data = self.data == 0 ? 0 : self.data->clone();
		}
		ImageResourceInfo & operator = (const ImageResourceInfo &self) {
			delete data;
			this->id   = self.id;
			this->name = self.name;
			this->size = self.size;
			this->data = self.data == 0 ? 0 : self.data->clone();
			return *this;
		}
    uint16_t id;         // 識別ID
		std::string name;    // 名前
		int size;            // サイズ
		IteratorBase *data;  // 参照
	};
	
	struct GlobalLayerMaskInfo {
		bool present = false;  // 空でない global layer mask info ブロックがあったか
		int overlayColorSpace = 0;
		int color1 = 0;
		int color2 = 0;
		int color3 = 0;
		int color4 = 0;
		int opacity = 0;
		int kind = 0;
	};

	struct LayerMask {
    bool present = false;  // マスクブロック (size>0) が存在したか
    bool hasReal = false;  // real/user mask (size>=36) を含むか
    bool edited  = false;  // フィールドが編集され、save 時にフィールドから再直列化するか
    // マスクパラメータ (density/feather)。 flags bit4 (parameters_applied) が
    // 立っているとき本体が続く。density は 0..255 (-1=不在)、feather は double。
    bool hasParameters = false;   // パラメータブロックが実在したか
    int  paramFlags = 0;          // パラメータ有無ビット (bit0:userDensity /
                                  // bit1:userFeather / bit2:vecDensity / bit3:vecFeather)
    int  userMaskDensity = -1;    // ユーザーマスク濃度 0..255 (-1=不在)
    double userMaskFeather = 0.0; // ユーザーマスクぼかし (px)
    bool hasUserFeather = false;
    int  vectorMaskDensity = -1;  // ベクタマスク濃度 0..255 (-1=不在)
    double vectorMaskFeather = 0.0;
    bool hasVectorFeather = false;
    int width;
    int height;
		int top;
		int left;
		int bottom;
		int right;
		int defaultColor;
		int flags;
		int realFlags;
		int realUserMaskBackground;
		int enclosingTop;
		int enclosingLeft;
		int enclosingBottom;
		int enclosingRight;
	};

	struct LayerBlendingChannel {
		int source;
		int dest;
	};

	struct LayerBlendingRange {
		bool present = false;  // blending range ブロック (size>0) が存在したか
		int grayBlendSource;
		int grayBlendDest;
		std::vector<LayerBlendingChannel> channels;
	};

	// 追加レイヤ情報
	struct AdditionalLayerInfo {
    AdditionalLayerInfo(int sigType, int key, int size, IteratorBase *data) : sigType(sigType), key(key), size(size), data(data) {
		}
		~AdditionalLayerInfo() {
			delete data;
		}
		AdditionalLayerInfo(const AdditionalLayerInfo &self) {
			this->sigType = self.sigType;
			this->key = self.key;
			this->size = self.size;
			this->data = self.data == 0 ? 0 : self.data->clone();
		}
		AdditionalLayerInfo & operator = (const AdditionalLayerInfo &self) {
			delete data;
			this->sigType = self.sigType;
			this->key = self.key;
			this->size = self.size;
			this->data = self.data == 0 ? 0 : self.data->clone();
			return *this;
		}
		int sigType;
		int key;
		int size;
		IteratorBase *data;
	};

	struct LayerExtraData {
		LayerExtraData() : rawBytes(0), maskRaw(0), blendRaw(0), tailRaw(0), useRawBytes(true) {}
		~LayerExtraData() { delete rawBytes; delete maskRaw; delete blendRaw; delete tailRaw; }
		LayerExtraData(const LayerExtraData &self)
		  : layerMask(self.layerMask),
		    layerBlendingRange(self.layerBlendingRange),
		    layerName(self.layerName),
		    additionalLayers(self.additionalLayers),
		    rawBytes(self.rawBytes ? self.rawBytes->clone() : 0),
		    maskRaw(self.maskRaw ? self.maskRaw->clone() : 0),
		    blendRaw(self.blendRaw ? self.blendRaw->clone() : 0),
		    tailRaw(self.tailRaw ? self.tailRaw->clone() : 0),
		    useRawBytes(self.useRawBytes) {}
		LayerExtraData &operator=(const LayerExtraData &self) {
		    if (this == &self) return *this;
		    layerMask = self.layerMask;
		    layerBlendingRange = self.layerBlendingRange;
		    layerName = self.layerName;
		    additionalLayers = self.additionalLayers;
		    delete rawBytes;
		    rawBytes = self.rawBytes ? self.rawBytes->clone() : 0;
		    delete maskRaw;
		    maskRaw = self.maskRaw ? self.maskRaw->clone() : 0;
		    delete blendRaw;
		    blendRaw = self.blendRaw ? self.blendRaw->clone() : 0;
		    delete tailRaw;
		    tailRaw = self.tailRaw ? self.tailRaw->clone() : 0;
		    useRawBytes = self.useRawBytes;
		    return *this;
		}

		LayerMask layerMask;
		LayerBlendingRange layerBlendingRange;
		std::string layerName;
    std::vector<AdditionalLayerInfo> additionalLayers;
		// extraSize 全域の生バイト (ラウンドトリップ save 用)。parse 時に
		// cloneRange(0, extraSize) でキャプチャ。
		IteratorBase *rawBytes;
		// layer mask / blending ranges サブブロックの生バイト (4byte 長の後の本体)。
		// extra data をフィールドから再構築 (改名等) する際、マスク/ブレンド範囲は
		// これをそのまま転送してバイト一致を保つ。空ブロックのときは 0。
		IteratorBase *maskRaw;
		IteratorBase *blendRaw;
		// 追加情報ブロックの並びの後ろに残ったバイト (書き手によっては最後の
		// ブロックの後ろに詰め物を置く)。フィールドから再構築するときも末尾に
		// そのまま付ける。無ければ 0。
		IteratorBase *tailRaw;
		// true: rawBytes をそのまま書き出す (未編集)。
		// false: フィールド (名前 + maskRaw/blendRaw + additionalLayers) から再構築。
		bool useRawBytes;
	};

	// チャンネル情報
	struct ChannelInfo {
		ChannelInfo(int id, int length) : id(id), length(length), imageData(0) {};
    ~ChannelInfo() { delete imageData; }
    ChannelInfo(const ChannelInfo &self) {
			this->id        = self.id;
			this->length    = self.length;
			this->imageData = self.imageData == 0 ? 0 : self.imageData->clone();
		}
		ChannelInfo & operator = (const ChannelInfo &self) {
			delete imageData;
			this->id        = self.id;
			this->length    = self.length;
			this->imageData = self.imageData == 0 ? 0 : self.imageData->clone();
			return *this;
		}
    
		int id;
		int length;
    IteratorBase *imageData;

    bool isMaskChannel() const { return (id == -3 || id == -2); }
	};

  // テキストレイヤの文字スタイルラン。EngineData の StyleRun/RunArray の
  // 1 エントリに対応し、length は本文の何文字分に適用されるか (RunLengthArray)。
  struct TextStyleRun {
    int         length;      // 適用文字数 (UTF-16 コードユニット)
    u16str      font;        // 解決済みフォント名 (FontSet を index で引いたもの)
    float       fontSize;    // pt
    float       color[4];    // RGBA 0..1 (EngineData の ARGB を並べ替えて格納)
    bool        hasColor;    // FillColor が指定されていたか
    int         tracking;    // トラッキング (字送り, 1/1000 em)
    int         kerning;     // 手動カーニング
    bool        autoKerning; // 自動カーニング (メトリクス/オプティカル) 有効
    bool        bold;        // FauxBold (合成ボールド)
    bool        italic;      // FauxItalic (合成イタリック)
    bool        underline;   // Underline
    bool        sizeInherited; // FontSize を既定 StyleSheet から継承したか。
                               // 継承分は nominal pt なので dpi/72 で px 化する
                               // (明示 run の FontSize は既に解決済み px)。内部用。
    // 以下は FontSize と同じ単位 (px。既定から継承した分は dpi/72 で換算済み)
    bool        autoLeading = true;  // 行送り自動 (AutoLeading)
    float       leading = 0.0f;      // 行送り (autoLeading=false のとき有効)
    float       baselineShift = 0.0f;
    bool        strikethrough = false;
    int         fontCaps = 0;        // 0=通常 1=スモールキャップス 2=オールキャップス
    int         fontBaseline = 0;    // 0=通常 1=上付き 2=下付き
    float       horizontalScale = 1.0f;  // 水平比率 (1.0 = 100%)
    float       verticalScale = 1.0f;    // 垂直比率
    bool        ligatures = true;
    // leading / baselineShift を既定から継承したか (bit0 / bit1)。内部用。
    unsigned    inheritedPxMask = 0;

    TextStyleRun()
      : length(0), fontSize(0.0f), color{0,0,0,1}, hasColor(false),
        tracking(0), kerning(0), autoKerning(false),
        bold(false), italic(false), underline(false), sizeInherited(false) {}
  };

  // 段落単位の情報 (EngineDict/ParagraphRun)。 段落は本文中の改行 (\r) 区切り。
  struct TextParagraph {
    int length;          // 段落の文字数 (UTF-16 コードユニット, RunLengthArray)
    int justification;   // 行揃え 0=左 1=右 2=中央 (3..=両端揃え系)
    // インデントと段落前後のアキ (FontSize と同じ単位、px)
    float firstLineIndent = 0.0f;
    float startIndent = 0.0f;
    float endIndent = 0.0f;
    float spaceBefore = 0.0f;
    float spaceAfter = 0.0f;
    float autoLeading = 1.2f;   // 自動行送りの倍率
    bool  hyphenate = false;
    // 上の 5 つの長さを既定から継承したか (bit0..4)。内部用。
    unsigned inheritedPxMask = 0;

    TextParagraph() : length(0), justification(0) {}
  };

  // テキストのワープ (TySh の warp descriptor)。style が "warpNone" ならワープ無し。
  struct TextWarp {
    bool present = false;
    std::string style;          // "warpNone" / "warpArc" / "warpFlag" など
    double value = 0;           // 曲げ (%)
    double perspective = 0;     // 水平方向のゆがみ (%)
    double perspectiveOther = 0;// 垂直方向のゆがみ (%)
    std::string rotate;         // "Hrzn" (水平) / "Vrtc" (垂直)
  };

  // テキストレイヤ情報 (追加レイヤ情報 'TySh' 由来)。
  struct TextLayerData {
    bool        present;         // テキストレイヤとしてパースできたか
    u16str      text;            // 本文全体 (改行は \r)
    double      transform[6];    // アフィン変換 xx,xy,yx,yy,tx,ty
    std::string orientation;     // "horizontal" / "vertical"
    int         justification;   // 段落の行揃え 0=左 1=右 2=中央 (先頭段落; 後方互換)
    std::vector<TextStyleRun> runs;
    std::vector<TextParagraph> paragraphs;  // 段落別 (行揃えが段落で変わる box text 用)
    TextWarp    warp;

    TextLayerData()
      : present(false), transform{1,0,0,1,0,0},
        justification(0) {}
  };

	// レイヤ情報
  class Data;
	struct LayerInfo {
    // レイヤの所属するpsd::Dataインスタンス
    Data *owner;
    
    // parsed raw data
    int width;
    int height;
		int top;
		int left;
		int bottom;
		int right;
		std::vector<ChannelInfo> channels;
		int blendModeKey;
		BlendMode blendMode;
		int opacity;
		int fill_opacity;
		int clipping;
		int flag;
		LayerExtraData extraData;
    
    // migrate from extra/addtionals
    int layerId;
    LayerType layerType;
		std::string layerName;
    u16str layerNameUnicode;
    // int layerNameId;
    // int foreignEffectId;
    // BlendMode folderBlendMode; // blendMode 上書きにしている。問題あれば分離

    // レイヤーカンプ情報
    std::map<int, LayerCompInfo> layerComps;
    Descriptor layerCompDesc; // ディスクリプタ形式で全メタデータを格納

    // テキストレイヤ情報 ('TySh' 由来)。layerType==TEXT のとき present=true。
    TextLayerData textData;

    // 親フォルダレイヤ
    LayerInfo *parent;
    // 親フォルダの layerList インデックス (-1 = トップレベル)。
    // processParsed / relinkGroups で設定。
    int parentIndex = -1;

    bool isTransparencyProtected() const { return (flag & (1 << 0)) != 0; }
    bool isVisible()               const { return (flag & (1 << 1)) == 0; }
    bool isObsolete()              const { return (flag & (1 << 2)) != 0; }
    bool isLaterVer5()             const { return (flag & (1 << 3)) != 0; }
    bool isPixelDataIrrelevant()   const { return (flag & (1 << 4)) != 0; }

    // ベクタマスク ('vmsk' / 'vsms')。processParsed で設定。
    VectorMask vectorMask;
    // スマートオブジェクト ('SoLd' / 'SoLE' / 'PlLd')。processParsed で設定。
    SmartObjectInfo smartObject;
	};
	
	/**
	 * PSD Infomation class
	 */
	class Data {
	public:
		// コンストラクタ
		Data()
			 : colorModeSize(0), colorModeIterator(0),
			   mergedAlpha(false), channelImageData(0),
			   globalLayerMaskInfoRaw(0),
			   layerAndMaskTrailing(0),
			   globalBlocksEnd(0), layerSourceKey(0), layerInfoRaw(0),
			   imageData(0)
		{
		}

		// デストラクタ
		virtual ~Data() {
			clearData();
		}

		// 保持データの消去
		virtual void clearData() {
			delete colorModeIterator; colorModeIterator = 0;
			delete channelImageData; channelImageData = 0;
			delete globalLayerMaskInfoRaw; globalLayerMaskInfoRaw = 0;
			delete layerAndMaskTrailing; layerAndMaskTrailing = 0;
			globalBlocks.clear(); globalBlocksEnd = 0;
			globalBlockPatches.clear();
			layerSourceKey = 0;
			layerInfoOrigSize = -1;
			layerMaskOrigSize = -1;
			hasGlobalMaskField = true;
			delete layerInfoRaw; layerInfoRaw = 0;
			delete imageData; imageData = 0;
		}


		// RLE の行バイト数テーブルの 1 行あたりのバイト数 (PSD は 2、PSB は 4)。
		int rowCountBytes() const { return header.isPSB() ? 4 : 2; }

		// レイヤをレイヤIDで取得
    LayerInfo *getLayerById(int layerId);

    // レイヤの親子関係 (parent / parentIndex) を layerList から再計算する。
    // 読み込み時は processParsed が呼ぶ。構造編集 (削除 / 移動 / 複製 / 挿入)
    // を行うと layerList の並びが変わって古くなるので、編集 API はこれを
    // 呼んでから戻る。
    void relinkGroups();

    // index のレイヤが占める layerList 上の範囲を返す。
    // 通常のレイヤは [index, index]。フォルダは対応する区切り (LAYER_TYPE_HIDDEN)
    // から自分自身までの塊 [divider, index] を返す (入れ子も内側に含む)。
    // 移動 / 削除でグループを 1 つの塊として扱うために使う。
    // 範囲外なら false。
    bool groupSpan(int index, int &startOut, int &countOut) const;

    // --- ツリービュー (読み取り専用の派生ビュー) ----------------------------
    //
    // layerList は PSD のファイル形式そのままの平坦な並びで、フォルダは
    // 「区切り (LAYER_TYPE_HIDDEN) と フォルダ (LAYER_TYPE_FOLDER) の 2 枚組」
    // として埋め込まれている。編集も保存も平坦リストが正なので、そこは変えない。
    // 一方、他形式と共通のインタフェースを組むときは平坦リストより木の方が
    // 素直なので、その見え方を派生ビューとして提供する。
    //
    // parentIndex に -1 を渡すと最上位を返す。並びは layerList と同じ
    // (下から上)。**区切りレイヤは符号化の都合なので結果に含めない。**
    std::vector<int> childIndices(int parentIndex) const;

		// ---------------------------------------
		// イメージリソース
		// ---------------------------------------

		// ヘッダ情報
		Header header;

		// カラーモード情報
		int colorModeSize;
		IteratorBase *colorModeIterator;
		
		// イメージリソース一覧
		std::vector<ImageResourceInfo> imageResourceList;
		
		// 合成α情報があるかどうか
		bool mergedAlpha;

		// レイヤ情報一覧
		std::vector<LayerInfo> layerList;

		// レイヤの構造編集 (削除/並べ替え/複製/他ファイルからのコピー) が行われたか。
		// false のうちは channelImageData の連結ブロブをそのまま書き出してバイト
		// 一致のラウンドトリップを保つ。true になると save() 時にチャンネルを
		// レイヤ毎に個別再構築する (連結ブロブは編集後のレイヤ順と合わないため)。
		bool layersDirty = false;

		// 読み込み時の layer info の長さ (長さフィールドの値)。-1 なら新規作成。
		// 保存時に、元と同じ形 (空のまま / 詰め物の有無) を保つために使う。
		int layerInfoOrigSize = -1;
		// 読み込み時の layer & mask 情報ぜんたいの長さ。0 (セクションごと空) の
		// ファイルは、レイヤを足さない限り空のまま書き戻す。
		int layerMaskOrigSize = -1;
		// layer & mask 情報に global layer mask info の長さフィールドがあったか。
		// 古い / 他社製のファイルはフィールドごと省くことがある。
		bool hasGlobalMaskField = true;

		// チャンネル画像データ
		IteratorBase *channelImageData;
		
		// global layer mask info
		GlobalLayerMaskInfo globalLayerMaskInfo;
		// global layer mask info ブロック (4 バイトサイズ後の本体) の生バイト。
		// ラウンドトリップ save 用。空ブロック (size=0) の場合は 0。
		IteratorBase *globalLayerMaskInfoRaw;
		// layer and mask info 全体の中で global layer mask info より後ろに
		// ある追加 info (Lr16/Lr32 などの secondary layer info) を未解釈の
		// まま保持。ラウンドトリップ save 用。
		IteratorBase *layerAndMaskTrailing;

		// layerAndMaskTrailing を追加情報ブロックの並びとして読んだ結果。
		// globalBlocksEnd 以降はブロックとして読めなかった残りで、書き出しでは
		// そのまま転送する。
		std::vector<GlobalBlockInfo> globalBlocks;
		int globalBlocksEnd;

		// 文書末尾の追加情報 (Txt2 など) の差し替え / 削除。キー → 新しい
		// ブロック全体 ('8BIM' + key + 長さ + 中身 + 詰め物)。空文字列なら削除。
		// 巨大な lnk2 などを丸ごとメモリへ写さずに済むよう、差し替えたブロック
		// だけを持ち、残りは元の範囲から転送する。
		std::map<int, std::string> globalBlockPatches;

		// レイヤ一覧の出どころ。0 なら通常の layer info。16/32bit 文書では
		// Photoshop はレイヤを末尾の 'Lr16' / 'Lr32' (まれに 'Layr') に置き、
		// layer info 本体は空にする。その場合ここにキーが入り、保存時は
		// そのブロックをレイヤ一覧から書き直す。
		int layerSourceKey;
		// layerSourceKey != 0 のときの、元の layer info 本体 (長さフィールドの
		// 後ろ)。空 (長さ 0) なら 0。保存時にそのまま書き戻す。
		IteratorBase *layerInfoRaw;
		
		// 合成済み画像データ
		IteratorBase *imageData;

    // 展開済みリソースデータ
    SliceResource      slice;       // スライス
    GridGuideResource  gridGuide;   // グリッド/ガイド
    ColorTable         colorTable;  // カラーテーブル(インデックスカラー用)
    std::vector<LayerComp> layerComps; // レイヤーカンプ
    std::vector<SavedPath> savedPaths; // 保存パス (2000〜2997) と作業パス (1025)。リソース順
    std::vector<LinkedFileInfo> linkedFiles; // スマートオブジェクトの埋め込み / リンクファイル
    std::vector<PatternInfo> patterns;       // 文書のパターン ('Patt' / 'Pat2' / 'Pat3')
    std::vector<AlphaChannelInfo> alphaChannels; // 色チャンネルの後ろの余分なチャンネル
    int lastAppliedCompId;             // 最終適用カンプ

  protected:
    bool processParsed();
	};
}
#endif
