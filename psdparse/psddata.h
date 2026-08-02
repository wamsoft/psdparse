#ifndef __psddata_h__
#define __psddata_h__

#include "psdbase.h"
#include "psddesc.h"

#include <vector>
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
	};

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
		LayerExtraData() : rawBytes(0), maskRaw(0), blendRaw(0), useRawBytes(true) {}
		~LayerExtraData() { delete rawBytes; delete maskRaw; delete blendRaw; }
		LayerExtraData(const LayerExtraData &self)
		  : layerMask(self.layerMask),
		    layerBlendingRange(self.layerBlendingRange),
		    layerName(self.layerName),
		    additionalLayers(self.additionalLayers),
		    rawBytes(self.rawBytes ? self.rawBytes->clone() : 0),
		    maskRaw(self.maskRaw ? self.maskRaw->clone() : 0),
		    blendRaw(self.blendRaw ? self.blendRaw->clone() : 0),
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
    bool        sizeInherited; // FontSize を既定 StyleSheet から継承したか。
                               // 継承分は nominal pt なので dpi/72 で px 化する
                               // (明示 run の FontSize は既に解決済み px)。内部用。

    TextStyleRun()
      : length(0), fontSize(0.0f), color{0,0,0,1}, hasColor(false),
        tracking(0), kerning(0), autoKerning(false), sizeInherited(false) {}
  };

  // 段落単位の情報 (EngineDict/ParagraphRun)。 段落は本文中の改行 (\r) 区切り。
  struct TextParagraph {
    int length;          // 段落の文字数 (UTF-16 コードユニット, RunLengthArray)
    int justification;   // 行揃え 0=左 1=右 2=中央 (3..=両端揃え系)

    TextParagraph() : length(0), justification(0) {}
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
    // 親フォルダの layerList インデックス (-1 = トップレベル)。processParsed で設定。
    int parentIndex = -1;

    bool isTransparencyProtected() const { return (flag & (1 << 0)) != 0; }
    bool isVisible()               const { return (flag & (1 << 1)) == 0; }
    bool isObsolete()              const { return (flag & (1 << 2)) != 0; }
    bool isLaterVer5()             const { return (flag & (1 << 3)) != 0; }
    bool isPixelDataIrrelevant()   const { return (flag & (1 << 4)) != 0; }
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
			delete imageData; imageData = 0;
		}


		// レイヤをレイヤIDで取得
    LayerInfo *getLayerById(int layerId);

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
		
		// 合成済み画像データ
		IteratorBase *imageData;

    // 展開済みリソースデータ
    SliceResource      slice;       // スライス
    GridGuideResource  gridGuide;   // グリッド/ガイド
    ColorTable         colorTable;  // カラーテーブル(インデックスカラー用)
    std::vector<LayerComp> layerComps; // レイヤーカンプ
    int lastAppliedCompId;             // 最終適用カンプ

  protected:
    bool processParsed();
	};
}
#endif
