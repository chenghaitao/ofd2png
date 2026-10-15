"""Generate a minimal but structurally honest OFD invoice for end-to-end testing.

Not a substitute for a real producer's file - it only proves the pipeline
(ZIP -> XML -> layout objects -> SVG -> resvg -> PNG) is wired correctly and
that CJK text reaches a system font.
"""

import zipfile

OUT = "dist/sample.ofd"

OFD_XML = """<?xml version="1.0" encoding="UTF-8"?>
<ofd:OFD xmlns:ofd="http://www.ofdspec.org/2016" DocType="OFD" Version="1.0">
  <ofd:DocBody>
    <ofd:DocInfo><ofd:DocID>sample-invoice</ofd:DocID></ofd:DocInfo>
    <ofd:DocRoot>Doc_0/Document.xml</ofd:DocRoot>
    <ofd:CustomData>
      <ofd:CustomData Name="发票号码">25312000000012345678</ofd:CustomData>
      <ofd:CustomData Name="开票日期">2026年10月15日</ofd:CustomData>
      <ofd:CustomData Name="合计金额">1234.56</ofd:CustomData>
      <ofd:CustomData Name="合计税额">160.49</ofd:CustomData>
      <ofd:CustomData Name="购买方纳税人识别号">91310000MA1FL1234X</ofd:CustomData>
      <ofd:CustomData Name="销售方纳税人识别号">91320000MA1FL5678Y</ofd:CustomData>
    </ofd:CustomData>
  </ofd:DocBody>
</ofd:OFD>
"""

DOCUMENT_XML = """<?xml version="1.0" encoding="UTF-8"?>
<ofd:Document xmlns:ofd="http://www.ofdspec.org/2016">
  <ofd:CommonData>
    <ofd:DocumentID>sample-invoice</ofd:DocumentID>
  </ofd:CommonData>
  <ofd:Pages>
    <ofd:Page ID="1" BaseLoc="Pages/Page_0/Content.xml"/>
  </ofd:Pages>
</ofd:Document>
"""

PUBLIC_RES_XML = """<?xml version="1.0" encoding="UTF-8"?>
<ofd:Res xmlns:ofd="http://www.ofdspec.org/2016">
  <ofd:Fonts>
    <ofd:Font ID="1" FontName="SimSun" FamilyName="SimSun"/>
    <ofd:Font ID="2" FontName="SimHei" FamilyName="SimHei"/>
  </ofd:Fonts>
</ofd:Res>
"""

# DeltaX carries an absolute advance per character (width included), which is
# the path the engine has to consume positionally rather than as a delta.
TITLE_TEXT = "电子发票（普通发票）"
NUMBER_TEXT = "发票号码：25312000000012345678"
AMOUNT_TEXT = "价税合计：￥1395.05"

def deltas(text, step):
    return " ".join([str(step)] * (len(text) - 1))

CONTENT_XML = f"""<?xml version="1.0" encoding="UTF-8"?>
<ofd:Page xmlns:ofd="http://www.ofdspec.org/2016">
  <ofd:Area>
    <ofd:PhysicalBox>0 0 210 140</ofd:PhysicalBox>
  </ofd:Area>
  <ofd:Content>
    <ofd:Layer ID="1">
      <ofd:PathObject ID="1" Boundary="10 10 190 120" LineWidth="0.6" Fill="false">
        <ofd:StrokeColor Value="0,0,0"/>
        <ofd:AbbreviatedData>M 0 0 L 190 0 L 190 120 L 0 120 Z</ofd:AbbreviatedData>
      </ofd:PathObject>
      <ofd:PathObject ID="2" Boundary="10 40 190 0" LineWidth="0.4" Fill="false">
        <ofd:StrokeColor Value="0,0,0"/>
        <ofd:AbbreviatedData>M 0 0 L 190 0</ofd:AbbreviatedData>
      </ofd:PathObject>
      <ofd:TextObject ID="10" Boundary="70 18 90 10" Font="2" Size="7">
        <ofd:TextCode X="0" Y="0">{TITLE_TEXT}</ofd:TextCode>
      </ofd:TextObject>
      <ofd:TextObject ID="11" Boundary="20 30 150 6" Font="1" Size="4.5">
        <ofd:TextCode X="0" Y="0" DeltaX="{deltas(NUMBER_TEXT, 5.2)}">{NUMBER_TEXT}</ofd:TextCode>
      </ofd:TextObject>
      <ofd:TextObject ID="12" Boundary="20 50 120 6" Font="1" Size="4.5">
        <ofd:TextCode X="0" Y="0">销售方名称：上海某某科技有限公司</ofd:TextCode>
      </ofd:TextObject>
      <ofd:TextObject ID="13" Boundary="20 62 120 6" Font="1" Size="4.5">
        <ofd:TextCode X="0" Y="0">购买方名称：北京某某贸易有限公司</ofd:TextCode>
      </ofd:TextObject>
      <ofd:TextObject ID="14" Boundary="140 100 60 8" Font="2" Size="6">
        <ofd:TextCode X="0" Y="0">{AMOUNT_TEXT}</ofd:TextCode>
      </ofd:TextObject>
    </ofd:Layer>
  </ofd:Content>
</ofd:Page>
"""

FILES = {
    "OFD.xml": OFD_XML,
    "Doc_0/Document.xml": DOCUMENT_XML,
    "Doc_0/PublicRes.xml": PUBLIC_RES_XML,
    "Doc_0/Pages/Page_0/Content.xml": CONTENT_XML,
}

with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
    for name, body in FILES.items():
        z.writestr(name, body)

print(f"wrote {OUT}")
