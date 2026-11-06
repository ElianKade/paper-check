# -*- coding: utf-8 -*-
"""生成 samples/ 目录下的测试语料。

语料由 8 篇中文短文组成，通过“原文 / 轻度改写 / 段落摘抄 / 抄袭副本 / 无关文本”
等不同组合，覆盖低、正常、高三种查重率区间：

    04 数据结构报告      → 低（与 05 互为副本，单看 04 只有与 05 重复）
    06 网络协议分析      → 低（无重复段落）
    08 学术诚信声明      → 低（短文本，无重复段落）
    03 机器学习信用评分  → 正常/警示（摘抄 01 的两个段落）
    07 操作系统笔记      → 正常/警示（摘抄 01 与 06 各一段）
    02 人工智能综述改写  → 高（与 01 大段重复）
    05 数据结构报告副本  → 高（与 04 完全相同）

依赖：python-docx 生成 DOCX；Microsoft Edge 无头模式打印 HTML 生成 PDF
（Chromium 会嵌入字体子集与 ToUnicode 映射，便于本项目自研 PDF 解析器提取中文）。
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from docx import Document
from docx.shared import Pt

EDGE_CANDIDATES = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]

# ---------------------------------------------------------------------------
# 语料正文：每篇文档由标题 + 若干段落组成
# ---------------------------------------------------------------------------

AI_PARAGRAPHS = [
    "人工智能是研究、开发用于模拟、延伸和扩展人的智能的理论、方法、技术及应用系统的一门新的技术科学。"
    "它试图了解智能的实质，并生产出一种新的能以人类智能相似的方式做出反应的智能机器。"
    "该领域的研究包括机器人、语言识别、图像识别、自然语言处理和专家系统等。",

    "人工智能的概念最早可以追溯到二十世纪五十年代。一九五六年夏天，一批年轻的科学家在达特茅斯学院聚会，"
    "共同探讨用机器模拟智能的一系列有关问题，并首次提出了人工智能这一术语，标志着人工智能学科的诞生。"
    "此后几十年间，该领域经历了数次高潮与低谷。",

    "机器学习是人工智能的核心分支，它研究计算机怎样模拟或实现人类的学习行为，以获取新的知识或技能，"
    "重新组织已有的知识结构使之不断改善自身的性能。按照学习方式的不同，机器学习可以分为监督学习、"
    "无监督学习、半监督学习和强化学习等类型。",

    "深度学习是机器学习的一个重要方向，它依靠多层神经网络自动提取数据的层次化特征，"
    "避免了传统方法中繁琐的人工特征工程。卷积神经网络在图像识别领域取得了突破性进展，"
    "循环神经网络和注意力机制则显著提升了自然语言处理的效果。",

    "在实际应用中，人工智能技术已经渗透到社会生活的方方面面。智能推荐系统根据用户的历史行为预测其兴趣偏好，"
    "语音助手能够理解并执行自然语言指令，自动驾驶汽车依靠多传感器融合感知周围环境，"
    "医学影像分析系统可以辅助医生发现早期病灶。",

    "与此同时，人工智能的发展也面临着诸多挑战。数据质量参差不齐会导致模型产生偏见，"
    "深度神经网络的决策过程缺乏可解释性，训练大规模模型需要消耗大量的计算资源和能源，"
    "算法的安全性以及用户隐私保护问题也日益受到关注。",

    "展望未来，人工智能将朝着更加通用、更加可信、更加绿色的方向发展。"
    "研究者需要在算法效率、模型可解释性和数据安全等方面持续投入，同时建立完善的法律法规和伦理规范，"
    "确保技术发展始终服务于人类福祉。",

    "综上所述，人工智能作为新一轮科技革命和产业变革的重要驱动力量，正在深刻改变人类的生产生活方式。"
    "只有坚持创新驱动与规范发展并重，才能让人工智能技术更好地造福社会。",
]

AI_REWRITTEN = [
    AI_PARAGRAPHS[0],

    "人工智能的思想源头可以回溯到二十世纪中期。一九五六年，一批青年学者在达特茅斯学院举行学术研讨，"
    "围绕机器智能的实现途径展开讨论，并在会议上第一次使用了人工智能这一名称，这被公认为该学科正式确立的标志。"
    "在随后的岁月里，这一领域经历过多次快速升温与相对沉寂的交替。",

    AI_PARAGRAPHS[2],
    AI_PARAGRAPHS[3],
    AI_PARAGRAPHS[4],

    "同时也要看到，人工智能在快速发展中仍然面临不少难题。训练数据的质量差异可能使模型输出带有偏见，"
    "深层神经网络的判断依据往往难以解释，大规模模型的训练与推理消耗了大量算力和电力，"
    "算法可靠性与个人隐私保护同样是学界和产业界共同关心的问题。",

    AI_PARAGRAPHS[6],
    AI_PARAGRAPHS[7],
]

CREDIT_SCORING = [
    "随着消费金融业务的快速发展，如何准确评估借款人的信用风险成为金融机构关注的核心问题。"
    "传统的信用评分模型主要依赖逻辑回归等线性方法，难以刻画特征之间复杂的交互关系，"
    "在客群差异较大的场景下容易出现精度不足的情况。",

    # 摘抄自《人工智能技术发展综述》第三段
    AI_PARAGRAPHS[2],

    # 摘抄自《人工智能技术发展综述》第四段
    AI_PARAGRAPHS[3],

    "本文以某消费金融平台脱敏后的十万条借贷记录为样本，构建了包含年龄、收入、负债率、"
    "历史逾期次数等三十余维特征的指标体系。在数据预处理阶段，对连续变量进行分箱和归一化处理，"
    "对类别变量采用目标编码，并对样本进行分层抽样以缓解正负样本不均衡的问题。",

    "实验采用五折交叉验证，以曲线下面积和区分度指标作为评价标准。结果显示，梯度提升树模型的"
    "曲线下面积达到零点八六，明显优于逻辑回归基线的零点七八。特征重要性分析表明，"
    "历史逾期次数和负债收入比对违约概率的影响最大，收入水平与学历的作用相对有限。",

    "研究同时发现，模型在不同客群上的表现存在明显差异，年轻客群的预测精度相对较低，"
    "可能与样本量偏少和行为数据稀疏有关。后续工作将引入迁移学习与联邦学习框架，"
    "在保护用户隐私的前提下提升模型的泛化能力，并尝试把图神经网络用于团伙欺诈的识别。",
]

DATA_STRUCTURE = [
    "本报告围绕校园图书借阅管理系统的设计与实现展开。系统需要支持图书信息维护、读者管理、"
    "借阅与归还、逾期提醒等基本业务，并要求在数据量持续增长时仍然保持较好的响应速度。",

    "在逻辑结构设计上，图书信息采用顺序表存储，以便通过下标快速访问；读者信息采用单链表组织，"
    "便于频繁的插入与删除；借阅记录按照时间顺序压入栈中，用于实现最近操作的撤销功能。"
    "各类结构之间通过读者编号和图书编号建立关联。",

    "核心算法方面，图书检索采用二分查找，前提是图书编号已经按升序排列，时间复杂度为对数级别；"
    "新书上架时调用插入排序维护有序性，在单次批量较小的情况下性能完全可以接受。",

    "为了实现热门图书推荐，系统统计每本图书在最近三十天内的借阅次数，"
    "使用小顶堆维护借阅次数最高的若干本图书，堆的大小固定为十，每次更新仅需对数时间的调整，"
    "避免了每次都做全量排序的开销。",

    "测试环节构造了一万条图书记录和五千条读者记录，分别测试了检索、插入、删除和推荐四类操作。"
    "实验结果表明，二分检索平均耗时约二十微秒，推荐列表生成的平均耗时低于一毫秒，"
    "整体性能满足课程设计提出的指标要求。",

    "通过本次课程设计，加深了对线性表、栈、查找和排序等基本数据结构的理解，"
    "也认识到合理选择数据组织方式对程序性能的重要影响。后续可以引入平衡二叉树或哈希表，"
    "进一步优化检索与去重环节的效率。",
]

NETWORK = [
    "计算机网络体系结构通常采用分层设计的思想，把复杂的通信问题分解为若干相对独立的子问题。"
    "国际标准化组织提出的开放系统互连参考模型包含七层，而互联网实际使用的体系结构则简化为四层。",

    "应用层直接面向用户进程，常见的协议包括超文本传输协议、文件传输协议、简单邮件传输协议和域名系统。"
    "超文本传输协议基于请求响应模式工作，客户端发送请求报文，服务器返回状态码与响应内容，"
    "无状态的特点使其易于扩展和部署缓存。",

    "传输层为应用进程提供端到端的通信服务。传输控制协议通过三次握手建立连接，"
    "利用序号、确认与重传机制保证数据的可靠传输，并通过滑动窗口实现流量控制；"
    "用户数据报协议则不保证可靠性，但具有开销小、延迟低的特点，适用于实时音视频等场景。",

    "网络层负责分组的路由与转发。互联网协议为每台主机分配唯一的地址，路由器根据路由表选择下一跳，"
    "当网络规模增大时采用子网划分与无类别域间路由来缓解地址紧张的问题，"
    "互联网控制报文协议则用于报告传输过程中的差错。",

    "数据链路层解决相邻结点之间的数据传输问题，通过帧定界、差错检测和介质访问控制保证链路的可靠使用。"
    "以太网采用载波监听多路访问与冲突检测机制协调共享信道的使用，交换机则通过地址表实现端口间的定向转发。",

    "网络安全是网络体系设计中不可忽视的环节。传输层安全协议在应用层与传输层之间提供加密与身份认证，"
    "防火墙和入侵检测系统则从边界与流量两个层面提升网络的整体防护能力，"
    "零信任架构进一步强调对每次访问请求进行持续验证。",
]

OPERATING_SYSTEM = [
    "操作系统是管理计算机硬件与软件资源的系统软件，它为用户和应用程序提供统一的接口。"
    "从资源管理的角度看，操作系统负责处理机管理、存储器管理、设备管理和文件管理四大功能，"
    "并通过中断与系统调用在用户态和内核态之间切换。",

    # 摘抄自《人工智能技术发展综述》第二段
    AI_PARAGRAPHS[1],

    "进程是程序在一次执行过程中的动态实体，具有就绪、运行和阻塞三种基本状态。"
    "进程调度算法包括先来先服务、短作业优先、时间片轮转和优先级调度，"
    "不同算法在吞吐量、响应时间与公平性之间各有取舍，实际系统往往采用多级反馈队列加以折中。",

    "内存管理采用分页机制把逻辑地址空间划分为固定大小的页，通过页表完成逻辑地址到物理地址的转换。"
    "为了缓解缺页带来的性能损失，系统引入多级页表和快表，并采用最近最少使用等页面置换算法，"
    "在局部性原理的支撑下取得了较好的命中率。",

    # 摘抄自《计算机网络协议分析》第三段
    NETWORK[2],

    "文件系统负责文件的组织、存取与保护。常见的目录结构有单级目录、两级目录和树形目录，"
    "索引结点把文件名与文件描述信息分离，既节省了目录项空间，又提高了检索效率，"
    "日志机制则保证系统崩溃后元数据的一致性。",

    "并发与同步是操作系统设计中的难点。信号量机制通过等待与唤醒操作实现进程间的互斥与同步，"
    "管程把共享变量与操作封装在一起，进一步降低了并发程序设计的复杂度，"
    "死锁的预防、避免与检测则需要在资源分配策略上做出权衡。",
]

INTEGRITY = [
    "学术诚信是科学研究的生命线。本人郑重承诺，在课程报告与毕业论文的撰写过程中，"
    "严格遵守学术规范，不抄袭、不剽窃他人成果，不伪造、篡改实验数据与调查结论。",

    "凡引用他人观点、数据与结论之处，均已在正文中明确标注出处，并在参考文献中列出完整信息。"
    "引用内容仅用于说明和讨论问题，不构成文章的主要部分，也不以改写等方式规避查重检测。",

    "本人已充分了解学校关于学术不端行为的认定标准与处理办法，愿意接受查重系统的检测与学术委员会的审查。"
    "如有违反上述承诺之处，本人愿意承担相应的学术责任与纪律处分。",
]

DOCUMENTS = [
    {
        "file": "01_人工智能技术发展综述.docx",
        "kind": "docx",
        "title": "人工智能技术发展综述",
        "paragraphs": AI_PARAGRAPHS,
        "note": "原文，作为后续改写与摘抄的对照基准",
    },
    {
        "file": "02_人工智能技术发展综述_改写版.docx",
        "kind": "docx",
        "title": "人工智能技术发展综述（改写版）",
        "paragraphs": AI_REWRITTEN,
        "note": "对 01 做轻度改写，预期查重率很高（约 85%）",
    },
    {
        "file": "03_机器学习算法在信用评分中的应用.pdf",
        "kind": "pdf",
        "title": "机器学习算法在信用评分中的应用",
        "paragraphs": CREDIT_SCORING,
        "note": "摘抄 01 的两个段落，预期查重率约 30%（警示区）",
    },
    {
        "file": "04_数据结构课程设计报告.docx",
        "kind": "docx",
        "title": "数据结构课程设计报告——校园图书借阅管理系统",
        "paragraphs": DATA_STRUCTURE,
        "note": "原创报告，与其他文档基本无重复（低查重率）",
    },
    {
        "file": "05_数据结构课程设计报告_副本.pdf",
        "kind": "pdf",
        "title": "数据结构课程设计报告——校园图书借阅管理系统",
        "paragraphs": DATA_STRUCTURE,
        "note": "04 的完整副本，预期查重率约 100%（高危）",
    },
    {
        "file": "06_计算机网络协议分析.pdf",
        "kind": "pdf",
        "title": "计算机网络协议分层与安全机制分析",
        "paragraphs": NETWORK,
        "note": "原创文档，与大部分文档无关（低查重率）",
    },
    {
        "file": "07_操作系统课程笔记.docx",
        "kind": "docx",
        "title": "操作系统课程笔记",
        "paragraphs": OPERATING_SYSTEM,
        "note": "摘抄 01 与 06 各一段，预期查重率约 20%（正常区间）",
    },
    {
        "file": "08_学术诚信承诺书.pdf",
        "kind": "pdf",
        "title": "学术诚信承诺书",
        "paragraphs": INTEGRITY,
        "note": "短文本，无重复段落（低查重率）",
    },
]

HTML_TEMPLATE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>{title}</title>
<style>
  @page {{ size: A4; margin: 22mm 20mm; }}
  body {{ font-family: "Microsoft YaHei", "SimSun", serif; font-size: 12pt; line-height: 1.9; color: #111; }}
  h1 {{ font-size: 18pt; text-align: center; margin: 0 0 8mm; font-weight: 600; }}
  p {{ margin: 0 0 4mm; text-indent: 2em; text-align: justify; }}
</style>
</head>
<body>
<h1>{title}</h1>
{body}
</body>
</html>
"""


def find_edge() -> str:
    """定位 Microsoft Edge 可执行文件，用于把 HTML 打印成 PDF。"""
    for candidate in EDGE_CANDIDATES:
        if os.path.isfile(candidate):
            return candidate
    found = shutil.which("msedge")
    if found:
        return found
    raise RuntimeError("未找到 Microsoft Edge，无法生成 PDF 测试语料")


def write_docx(path: Path, document: dict) -> None:
    """用 python-docx 写出一篇带标题与段落的 Word 文档。"""
    doc = Document()
    style = doc.styles["Normal"]
    style.font.name = "宋体"
    style.font.size = Pt(12)

    doc.add_heading(document["title"], level=0)
    for paragraph in document["paragraphs"]:
        doc.add_paragraph(paragraph)
    doc.save(str(path))


def write_html(html_path: Path, document: dict) -> None:
    """根据文档内容生成排版用的 HTML，供 Edge 打印为 PDF。"""
    body = "\n".join("<p>%s</p>" % paragraph for paragraph in document["paragraphs"])
    html = HTML_TEMPLATE.format(title=document["title"], body=body)
    html_path.write_text(html, encoding="utf-8")


def write_pdf(edge: str, html_path: Path, pdf_path: Path, profile_dir: Path) -> None:
    """调用 Edge 无头模式把 HTML 打印成 PDF（内嵌字体子集 + ToUnicode 映射）。"""
    if pdf_path.exists():
        pdf_path.unlink()
    command = [
        edge,
        "--headless=new",
        "--disable-gpu",
        "--no-first-run",
        "--disable-extensions",
        "--user-data-dir=%s" % profile_dir,
        "--no-pdf-header-footer",
        "--print-to-pdf=%s" % pdf_path,
        html_path.as_uri(),
    ]
    result = subprocess.run(command, capture_output=True, timeout=120)
    if not pdf_path.exists():
        raise RuntimeError(
            "Edge 打印 PDF 失败：%s\n%s" % (pdf_path.name, result.stderr.decode("utf-8", "ignore"))
        )


def main() -> int:
    """遍历 DOCUMENTS，逐个生成 DOCX 或 PDF 测试文档。"""
    root = Path(__file__).resolve().parent.parent
    samples_dir = root / "samples"
    work_dir = root / "build" / "samples_html"
    samples_dir.mkdir(parents=True, exist_ok=True)
    work_dir.mkdir(parents=True, exist_ok=True)

    edge = find_edge()
    profile_dir = Path(tempfile.mkdtemp(prefix="pc_edge_profile_"))

    print("输出目录：%s" % samples_dir)
    for document in DOCUMENTS:
        target = samples_dir / document["file"]
        if document["kind"] == "docx":
            write_docx(target, document)
        else:
            html_path = work_dir / (target.stem + ".html")
            write_html(html_path, document)
            write_pdf(edge, html_path, target, profile_dir)
        size = target.stat().st_size
        chars = sum(len(paragraph) for paragraph in document["paragraphs"])
        print("  %-46s %7d 字节  %5d 字  %s" % (document["file"], size, chars, document["note"]))

    shutil.rmtree(profile_dir, ignore_errors=True)
    print("\n共生成 %d 篇测试文档。" % len(DOCUMENTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
