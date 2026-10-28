"""How a region sits on the control it names.

Drawn from the 80 redrawn samples in `training_data` (browse them with
`campcat/sources/py/view_samples.py`). Three facts decided the text below:

* 12 of the 21 shape changes were a wide diamond redrawn as a box. Every
  diamond a human kept was within 1.12 of square; every one replaced was at
  least 1.86 times wider than tall. The model was naming the shape it meant,
  not the shape of the control.
* A redrawn region was a median 0.69 of the proposal's area: the proposal
  carries margin the control does not have.
* The proposals that stayed boxes were flush at the top and spilled on the
  right (median 17px) and left (8px).

Both agents that propose a region, and the check that adjusts one, read this
text, so they cannot disagree about what a good region is.
"""

REGION_RULES = (
    "判断一个区域对不对，按这个顺序看：控件是文字时先看文字自身的边界，"
    "然后是形状，最后才是区域相对控件的尺寸。其余都是这三件事的细节。"
    "文字优先：要裁的是字本身，不是字坐着的那个东西。否则形状优先——"
    "形状对了、偏几个像素也还能匹配；形状错了，框得再紧也匹配不上。\n"
    "  * 控件是文字时，先看文字边界。一个标签、一行菜单、一个按钮文案，"
    "代表的就是它自己的墨迹：区域的边落在第一笔和最后一笔上，"
    "是一个恰好框住那些像素的方块。文字底下的按钮、胶囊、面板都不算控件，"
    "为了好看留的边距也不算——匹配器比的是像素，不是意思。\n"
    "    对：文案「开始游戏」给出一个四边贴着字形的方块。\n"
    "    错：把那句话外面整个红色按钮都框进去。它的填充和边框会随状态变化"
    "——悬停、置灰、换主题——而字形不变。\n"
    "  * 形状跟着控件的外轮廓走，不跟着它的含义走。先问这个控件真实的外轮廓"
    "是哪一种，然后挑唯一匹配的那个形状：四条直边 -> 方块；一圈不间断的圆弧 "
    "-> 圆；立在一个角上的正方形 -> 菱形。\n"
    "    对：旋转过的正方形用菱形——四个顶点落在它四个角上，宽高相差不超过"
    "三分之一。\n"
    "    对：圆形控件用圆，矩形、胶囊、横条、标签用方块。\n"
    "    错：把一个 240x60 的横条写成菱形。它的角就是横条自己的端头，"
    "菱形把这些端头丢掉了。又长又扁就是方块。\n"
    "    错：圆形控件还留着方块，或者直边的小图标用圆——形状会写出一张 alpha "
    "遮罩，用错了要么把控件本来有的角落丢掉，要么把它本来没有的背景留下。\n"
    "  * 尺寸就是控件自身的范围：区域盖住它的全部，且只盖住它。每一条边都落在"
    "控件自己的像素上。不要沿用第一次猜出来的边距：重画过的区域通常只有它纠正"
    "的那个提议面积的 0.7，让出去的那条边一般是右边或下边。\n"
    "    对：每条边都落在控件自己的边框上，中间不夹一条背景。\n"
    "    错：边飘在控件外面，看起来更好看。匹配器会把这些背景一起打分。\n"
    "  * 边界和尺寸都不能让步，也不能互相交换。\n"
    "    绝不要切进控件里面：一侧多一个像素的背景，比削掉一个角便宜。\n"
    "    对：边正好落在两个像素之间时，选能把控件留全的那一个。\n"
    "    错：一直往里缩，直到控件最外一圈没了，模板只剩下那块永远不变的部分。\n"
)
