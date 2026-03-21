// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2019-2026 XMuli & Contributors
// SPDX-GitHub: https://github.com/XMuli/ChineseChess
// SPDX-Author: XMuli <xmulitech@gmail.com>

#include "ChessBoard.h"
#include "ui_ChessBoard.h"
#include <mutex>

ChessBoard::ChessBoard(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::ChessBoard)
{
    init();
    m_bIsTcpServer = true;
    m_bReverseView = false;

    //计时器部分
    m_timer = new QTimer;  //初始化定时器
    m_timeRecord  = new QTime(0, 0, 0); //初始化时间
    m_bIsStart = false;  //初始为还未计时
    connect(m_timer,SIGNAL(timeout()),this,SLOT(updateTime()));

    m_pAbout = new AboutAuthor();

    this->setWindowIcon(QIcon(":/images/chess.svg"));
    ui->setupUi(this);

    // 交互只依赖于 ChessBoard，自身绘制用的 QLabel 需要放行鼠标事件
    if (ui->label) {
        ui->label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

    updateViolationDisplay();
}

ChessBoard::~ChessBoard()
{
    delete ui;
}

void ChessBoard::init()
{
    for(int i = 0; i<32; i++)
        m_ChessPieces[i].init(i);

    m_ChessSteps.clear(); //重置步数
    m_nSelectID = -1;
    m_nCheckedID = -1;
    m_bIsRed = true;
    m_bIsOver = false;
    m_bIsShowStep = true;
    m_bTimerAutoStarted = false;
    errCnt[0] = errCnt[1] = 0;
    clearLastGameResult();
    resetRuleTracking();
}

bool ChessBoard:: isRed(int id)
{
    return m_ChessPieces[id].m_bRed;
}


void ChessBoard:: killStone(int id)
{
    if(id== -1)
        return;
    m_ChessPieces[id].m_bDead= true;
}

void ChessBoard:: reliveStone(int id)
{
    if(id== -1)
        return;
    m_ChessPieces[id].m_bDead= false;
}

void ChessBoard:: moveStone(int moveid, int row, int col)
{
    m_ChessPieces[moveid].m_nRow= row;
    m_ChessPieces[moveid].m_nCol= col;

    m_bIsRed= !m_bIsRed;   //换边
}

bool ChessBoard::sameColor(int moveId,int killId)
{
    if(moveId== -1 || killId== -1)
        return false;

    return isRed(moveId)== isRed(killId);
}

bool ChessBoard::isDead(int id)
{
    if(id == -1)
        return true;

    return m_ChessPieces[id].m_bDead;
}

int ChessBoard::getStoneId(int row, int col)
{
    for(int i=0; i<32; ++i)
    {
        if(m_ChessPieces[i].m_nRow == row && m_ChessPieces[i].m_nCol == col && !isDead(i))
            return i;
    }

    return -1;
}

int ChessBoard::getStoneCountAtLine(int row1, int col1, int row2, int col2)
{
    int ret = 0;
    if(row1 != row2 && col1 != col2)
        return -1;
    if(row1 == row2 && col1 == col2)
        return -1;

    if(row1 == row2)
    {
        int min  = col1 < col2 ? col1 : col2;
        int max = col1 < col2 ? col2 : col1;
        for(int col = min+1; col<max; ++col)
        {
            if(getStoneId(row1, col) != -1)
                ++ret;
        }
    }
    else
    {
        int min = row1 < row2 ? row1 : row2;
        int max = row1 < row2 ? row2 : row1;
        for(int row = min+1; row<max; ++row)
        {
            if(getStoneId(row, col1) != -1)
                ++ret;
        }
    }

    return ret;
}

void ChessBoard::whoWin()  //谁胜谁负
{
    QString message;
    detectGameResult(true, message);
    presentLastGameResult();
}
int ChessBoard:: relation(int row1,int col1,int row2,int col2)
{
    // 原坐标(row1,col1)与目标坐标(row2,col2)的关系
    // 使用原坐标与目标坐标的行相减的绝对值乘以10 加上原坐标与目标坐标的列相减的绝对值
    // 作为关系值
    // 关系值用于判断是否符合棋子移动规则
    return abs(row1-row2)*10+ abs(col1-col2);
}

//是否选中该枚棋子。pt为输入参数; row， col为输出参数
bool ChessBoard::isChecked(QPointF pt, int &row, int &col)
{
    for (row = 0; row <= 9; row++) { // 10行
        for (col = 0; col <= 8; col++) { // 9列
            QPointF temp = center(row, col);
            qreal dx = temp.x() - pt.x();  // 使用 qreal
            qreal dy = temp.y() - pt.y();  // 使用 qreal
            if (dx * dx + dy * dy < m_nR * m_nR) {
                return true;
            }
        }
    }
    return false;
}


//象棋的棋盘的坐标转换成界面坐标
QPointF ChessBoard::center(int row, int col)
{
    const int displayRow = m_bReverseView ? (9 - row) : row;
    const int displayCol = m_bReverseView ? (8 - col) : col;
    QPointF rePoint;
    //这里注意坐标的转换
    rePoint.setY(displayRow * m_nD + m_nOffSet);  // 使用 setY
    rePoint.setX(displayCol * m_nD + m_nOffSet);  // 使用 setX

    return rePoint;
}

//重载:坐标转换
QPointF ChessBoard::center(int id)
{
    return center(m_ChessPieces[id].m_nRow, m_ChessPieces[id].m_nCol);
}

void ChessBoard::setPerspectiveFlipped(bool flipped)
{
    if (m_bReverseView == flipped)
        return;

    m_bReverseView = flipped;
    update();
}

bool ChessBoard::boardTransform(QPointF& origin, qreal& side) const
{
    if (!ui || !ui->label)
        return false;

    const QPoint topLeft = ui->label->mapTo(this, QPoint(0, 0));
    const QSize boardSize = ui->label->size();
    if (boardSize.isEmpty())
        return false;

    const qreal width = boardSize.width();
    const qreal height = boardSize.height();
    side = qMin(width, height);
    if (side <= 0.0)
        return false;

    origin = QPointF(topLeft);
    origin.rx() += (width - side) / 2.0;
    origin.ry() += (height - side) / 2.0;
    return true;
}

void ChessBoard::paintEvent(QPaintEvent *event)
{
    QMainWindow::paintEvent(event);

    QPointF boardOrigin;
    qreal boardSide = 0.0;
    if (!boardTransform(boardOrigin, boardSide))
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 填充整个棋盘区域背景（label 范围），深木色作为桌面底色
    const QPoint labelTopLeft = ui->label->mapTo(this, QPoint(0, 0));
    QRectF fullBg(labelTopLeft.x(), labelTopLeft.y(),
                  ui->label->width(), ui->label->height());
    QLinearGradient outerBg(fullBg.topLeft(), fullBg.bottomRight());
    outerBg.setColorAt(0, QColor(160, 120, 80));
    outerBg.setColorAt(1, QColor(130, 95, 60));
    painter.setPen(Qt::NoPen);
    painter.setBrush(outerBg);
    painter.drawRect(fullBg);

    painter.save();
    painter.translate(boardOrigin);
    const qreal scale = boardSide / 960.0;
    painter.scale(scale, scale);

        m_nOffSet = 60.0;  //距离界面的边距
        m_nD = 90.0;       //间距为50px
        m_nR = m_nD/2.0;   //棋子半径为d/2

        //*******************绘画棋盘*******************
        // 棋盘背景：木纹色渐变
        QRectF boardRect(m_nOffSet - m_nR, m_nOffSet - m_nR,
                         8 * m_nD + 2 * m_nR, 9 * m_nD + 2 * m_nR);
        QLinearGradient bgGrad(boardRect.topLeft(), boardRect.bottomRight());
        bgGrad.setColorAt(0, QColor(222, 184, 135));
        bgGrad.setColorAt(1, QColor(205, 170, 125));
        painter.setPen(Qt::NoPen);
        painter.setBrush(bgGrad);
        painter.drawRoundedRect(boardRect, 6, 6);

        // 棋盘线条：深棕色
        QPen boardPen(QColor(101, 67, 33), 2.0);
        painter.setPen(boardPen);

        //绘画10条横线
        for(int i = 0; i <= 9; i++)
            painter.drawLine(QPointF(m_nOffSet, m_nOffSet+i*m_nD), QPointF(m_nOffSet+8*m_nD, m_nOffSet+i*m_nD));

        //绘画9条竖线
        for(int i = 0; i <= 8; i++)
        {
            if(i==0 || i==8)
            {
                painter.drawLine(QPointF(m_nOffSet+i*m_nD, m_nOffSet), QPointF(m_nOffSet+i*m_nD, m_nOffSet+9*m_nD));
            }
            else
            {
                painter.drawLine(QPointF(m_nOffSet+i*m_nD, m_nOffSet), QPointF(m_nOffSet+i*m_nD, m_nOffSet+4*m_nD));
                painter.drawLine(QPointF(m_nOffSet+i*m_nD, m_nOffSet+5*m_nD), QPointF(m_nOffSet+i*m_nD, m_nOffSet+9*m_nD));
            }
        }

        //绘画4条斜线（九宫格）
        painter.drawLine(QPointF(m_nOffSet+3*m_nD, m_nOffSet), QPointF(m_nOffSet+5*m_nD, m_nOffSet+2*m_nD));
        painter.drawLine(QPointF(m_nOffSet+3*m_nD, m_nOffSet+2*m_nD), QPointF(m_nOffSet+5*m_nD, m_nOffSet));
        painter.drawLine(QPointF(m_nOffSet+3*m_nD, m_nOffSet+7*m_nD), QPointF(m_nOffSet+5*m_nD, m_nOffSet+9*m_nD));
        painter.drawLine(QPointF(m_nOffSet+3*m_nD, m_nOffSet+9*m_nD), QPointF(m_nOffSet+5*m_nD, m_nOffSet+7*m_nD));

        // 绘制星位标记（炮位和兵/卒位的十字花）
        drawStarMarks(painter);

        // 楚河汉界文字
        QRectF rect1(m_nOffSet+m_nD,   m_nOffSet+4*m_nD, m_nD, m_nD);
        QRectF rect2(m_nOffSet+2*m_nD, m_nOffSet+4*m_nD, m_nD, m_nD);
        QRectF rect3(m_nOffSet+5*m_nD, m_nOffSet+4*m_nD, m_nD, m_nD);
        QRectF rect4(m_nOffSet+6*m_nD, m_nOffSet+4*m_nD, m_nD, m_nD);
        painter.setPen(QColor(120, 80, 40, 180));
        painter.setFont(QFont("FangSong", m_nR * 5 / 6, 800));
        painter.drawText(rect1, "楚", QTextOption(Qt::AlignCenter));
        painter.drawText(rect2, "河", QTextOption(Qt::AlignCenter));
        painter.drawText(rect3, "汉", QTextOption(Qt::AlignCenter));
        painter.drawText(rect4, "界", QTextOption(Qt::AlignCenter));

        //*******************绘画棋子*******************
        //绘制上次移动棋子的起止位置
        if(m_bIsShowStep)
            drawLastStep(painter,m_ChessSteps);

        for(int i = 0; i < 32; i++)
            drawChessPieces(painter, i);

        //绘制文本棋谱
        painter.restore();
        drawTextStep();
}

void ChessBoard::drawChessPieces(QPainter &painter, int id)   //绘画单个具体的棋子
{
    if (isDead(id))
        return;

    QPointF c = center(id);
    QRectF rect(c.x()-m_nR, c.y()-m_nR, m_nD, m_nD);

    // 棋子阴影
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 40));
    painter.drawEllipse(QPointF(c.x()+2, c.y()+2), m_nR, m_nR);

    // 棋子底色：径向渐变
    QRadialGradient pieceGrad(c, m_nR);
    if (id < 16) { // 黑方
        pieceGrad.setColorAt(0, QColor(80, 80, 80));
        pieceGrad.setColorAt(0.8, QColor(45, 45, 45));
        pieceGrad.setColorAt(1, QColor(30, 30, 30));
    } else { // 红方
        pieceGrad.setColorAt(0, QColor(210, 60, 60));
        pieceGrad.setColorAt(0.8, QColor(160, 30, 30));
        pieceGrad.setColorAt(1, QColor(120, 15, 15));
    }
    painter.setBrush(pieceGrad);

    // 外圈边框
    if (m_nSelectID == id)
        painter.setPen(QPen(QColor(255, 215, 0), 3.0));  // 选中：金色发光
    else
        painter.setPen(QPen(QColor(180, 150, 100), 2.5)); // 默认：木色边框
    painter.drawEllipse(c, m_nR, m_nR);

    // 内圈装饰环
    painter.setPen(QPen(QColor(200, 175, 130, 160), 1.2));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(c, m_nR * 0.82, m_nR * 0.82);

    // 棋子文字
    painter.setFont(QFont("FangSong", m_nR * 5 / 6, 2700));
    if (id < 16)
        painter.setPen(QColor(220, 220, 220));  // 黑方：浅灰文字
    else
        painter.setPen(QColor(255, 230, 200));  // 红方：暖白文字

    painter.drawText(rect, m_ChessPieces[id].getnName(m_ChessPieces[id].m_bRed), QTextOption(Qt::AlignCenter));
}

void ChessBoard::drawStarMarks(QPainter &painter)
{
    // 星位标记位置：炮位(row=2,col=1/7; row=7,col=1/7) 和 兵/卒位
    // 兵位: row=3,col=0/2/4/6/8; row=6,col=0/2/4/6/8
    struct StarPos { int row; int col; };
    StarPos positions[] = {
        {2,1},{2,7},{7,1},{7,7},  // 炮位
        {3,0},{3,2},{3,4},{3,6},{3,8},  // 黑方兵位
        {6,0},{6,2},{6,4},{6,6},{6,8}   // 红方卒位
    };

    const qreal len = m_nD * 0.15;  // 标记线长度
    const qreal gap = m_nD * 0.08;  // 与交叉点的间距
    painter.setPen(QPen(QColor(101, 67, 33), 1.5));

    for (const auto& pos : positions) {
        qreal cx = m_nOffSet + pos.col * m_nD;
        qreal cy = m_nOffSet + pos.row * m_nD;

        // 右下角标记（边界列除外右侧）
        if (pos.col < 8) {
            painter.drawLine(QPointF(cx+gap, cy+gap), QPointF(cx+gap+len, cy+gap));
            painter.drawLine(QPointF(cx+gap, cy+gap), QPointF(cx+gap, cy+gap+len));
            painter.drawLine(QPointF(cx+gap, cy-gap), QPointF(cx+gap+len, cy-gap));
            painter.drawLine(QPointF(cx+gap, cy-gap), QPointF(cx+gap, cy-gap-len));
        }
        // 左侧标记（边界列除外左侧）
        if (pos.col > 0) {
            painter.drawLine(QPointF(cx-gap, cy+gap), QPointF(cx-gap-len, cy+gap));
            painter.drawLine(QPointF(cx-gap, cy+gap), QPointF(cx-gap, cy+gap+len));
            painter.drawLine(QPointF(cx-gap, cy-gap), QPointF(cx-gap-len, cy-gap));
            painter.drawLine(QPointF(cx-gap, cy-gap), QPointF(cx-gap, cy-gap-len));
        }
    }
}

void ChessBoard:: drawLastStep(QPainter &painter,QVector<ChessStep*>& steps)
{
    if (steps.size() == 0)
        return;

    // 使用 QPointF
    QPointF stepFrom = center(steps.last()->m_nRowFrom, steps.last()->m_nColFrom);
    QPointF stepTo = center(steps.last()->m_nRowTo, steps.last()->m_nnColTo);  // 假设有 stepTo，根据完整代码添加

    // 假设原有绘制代码，使用 QRectF
    painter.save();
    painter.setPen(QPen(Qt::red, 3));  // 示例：设置红色笔，粗细3

    // 绘制起始位置矩形（使用 QRectF）
    QRectF rectFrom(stepFrom.x() - m_nR, stepFrom.y() - m_nR, m_nR * 2.0, m_nR * 2.0);
    painter.drawRect(rectFrom);

    // 绘制目标位置矩形（使用 QRectF）
    QRectF rectTo(stepTo.x() - m_nR, stepTo.y() - m_nR, m_nR * 2.0, m_nR * 2.0);
    painter.drawRect(rectTo);

    painter.restore();
}

void ChessBoard::drawTextStep()
{
    ui->labelTextStep->setText(textStepRecord);
}

// true 产生"对将" 情景了；false 无"对将"情况
bool ChessBoard::hongMenFeast()
{
    if (m_ChessPieces[4].m_bDead || m_ChessPieces[20].m_bDead)
        return false;

    int colBlack = m_ChessPieces[4].m_nCol;
    int colRed = m_ChessPieces[20].m_nCol;
    int rowBlack = m_ChessPieces[4].m_nRow;
    int rowRed = m_ChessPieces[20].m_nRow;

    bool bColEmpty = true;
    if (colBlack == colRed){
        for (int row = rowBlack + 1; row < rowRed ; ++row) {
            if (havePieces(row, colBlack))
                bColEmpty = false;  // 将之间有棋子；非此列为空
        }
    } else {
        bColEmpty = false;
    }

    return bColEmpty;
}

// 判断某格子是否有棋子在其上
bool ChessBoard::havePieces(int row, int col)
{
    for (auto pieces : m_ChessPieces) {
        if (pieces.m_bDead)
            continue;

        if (pieces.m_nRow == row && pieces.m_nCol == col)
            return true;
    }

    return false;
}

// 胜负已分，重置
void ChessBoard::reset()
{
    m_Chessvoice.voiceWin();
    m_bIsOver = true;
    //游戏结束 则计时停止 & 计时控制按钮不再可用 直到用户重新游戏
    if(m_bIsStart)
    {
        pauseGameTimer();
    }

    ui->pushButton_start->setText("开始");
    ui->pushButton_start->setEnabled(false);
    m_bTimerAutoStarted = false;
}

void ChessBoard::startGameTimer()
{
    if (!m_timer || m_bIsStart)
        return;

    m_timer->start(1000);
    m_bIsStart = true;
    ui->pushButton_start->setText("暂停");
}

void ChessBoard::pauseGameTimer()
{
    if (!m_timer || !m_bIsStart)
        return;

    m_timer->stop();
    m_bIsStart = false;
    ui->pushButton_start->setText("继续");
}

void ChessBoard::autoStartTimerIfNeeded()
{
    if (m_bIsOver || m_bTimerAutoStarted)
        return;

    startGameTimer();
    m_bTimerAutoStarted = true;
}

void ChessBoard::winMessageBox(QString title, QString msg)
{
    QMessageBox message(QMessageBox::Information, title, msg);
    message.setIconPixmap(QPixmap(":/images/win.jpg"));
    message.setFont(QFont("FangSong", 16, QFont::Bold));
    message.exec();
}

QPointF ChessBoard::getRealPoint(QPointF pt)
{
    QPointF origin;
    qreal side = 0.0;
    if (!boardTransform(origin, side) || side <= 0.0)
        return QPointF(-1.0, -1.0);

    const QPointF relative = pt - origin;
    QPointF realPt;
    realPt.setX(relative.x() * 960.0 / side);
    realPt.setY(relative.y() * 960.0 / side);
    return realPt;
}

bool ChessBoard:: isGeneral()
{
    return isSideInCheck(m_bIsRed);
}

bool ChessBoard::isSideInCheck(bool redSide)
{
    const int generalId = redSide ? 20 : 4;
    if (m_ChessPieces[generalId].m_bDead)
        return false;

    const int row = m_ChessPieces[generalId].m_nRow;
    const int col = m_ChessPieces[generalId].m_nCol;

    for (int i = 0; i < 32; ++i) {
        if (m_ChessPieces[i].m_bDead || m_ChessPieces[i].m_bRed == redSide)
            continue;

        if (canMove(i, generalId, row, col))
            return true;
    }

    return false;
}

bool ChessBoard::wouldCauseSelfCheck(int moveId, int killId, int row, int col)
{
    const bool moverRed = isRed(moveId);
    const int oldRow = m_ChessPieces[moveId].m_nRow;
    const int oldCol = m_ChessPieces[moveId].m_nCol;
    const bool killedWasDead = (killId == -1) ? true : m_ChessPieces[killId].m_bDead;

    if (killId != -1)
        m_ChessPieces[killId].m_bDead = true;

    m_ChessPieces[moveId].m_nRow = row;
    m_ChessPieces[moveId].m_nCol = col;

    const bool checked = isSideInCheck(moverRed);

    m_ChessPieces[moveId].m_nRow = oldRow;
    m_ChessPieces[moveId].m_nCol = oldCol;
    if (killId != -1)
        m_ChessPieces[killId].m_bDead = killedWasDead;

    return checked;
}

bool ChessBoard::hasAnyLegalMove(bool redSide)
{
    const bool previousTurn = m_bIsRed;
    m_bIsRed = redSide;

    for (int id = 0; id < 32; ++id) {
        if (m_ChessPieces[id].m_bDead || m_ChessPieces[id].m_bRed != redSide)
            continue;

        for (int row = 0; row < 10; ++row) {
            for (int col = 0; col < 9; ++col) {
                const int killId = getStoneId(row, col);
                if (killId != -1 && sameColor(id, killId))
                    continue;
                if (!canMove(id, killId, row, col))
                    continue;
                if (wouldCauseSelfCheck(id, killId, row, col))
                    continue;

                m_bIsRed = previousTurn;
                return true;
            }
        }
    }

    m_bIsRed = previousTurn;
    return false;
}

void ChessBoard::showNetworkGui(const bool &show)
{
    ui->networkGroup->setVisible(show);
    if (ui->violationGroup) {
        ui->violationGroup->setVisible(show);
    }
}

void ChessBoard::updateViolationDisplay()
{
    if (!ui)
        return;

    if (ui->labelViolationRedValue) {
        ui->labelViolationRedValue->setText(QString("%1/%2").arg(errCnt[1]).arg(maxErrCnt));
    }
    if (ui->labelViolationBlackValue) {
        ui->labelViolationBlackValue->setText(QString("%1/%2").arg(errCnt[0]).arg(maxErrCnt));
    }
}

ChessBoard::GameResult ChessBoard::lastGameResult() const
{
    return m_lastGameResult;
}

QString ChessBoard::lastGameMessage() const
{
    return m_lastGameMessage;
}

void ChessBoard::clearLastGameResult()
{
    m_lastGameResult = GameResult::None;
    m_lastGameMessage.clear();
}

void ChessBoard::presentLastGameResult()
{
    if (m_lastGameResult == GameResult::None || m_lastGameMessage.isEmpty())
        return;

    const QString message = m_lastGameMessage;
    reset();
    winMessageBox("提示", message);
    clearLastGameResult();
}

QString ChessBoard::boardSignature() const
{
    QStringList parts;
    parts.reserve(33);
    parts.append(m_bIsRed ? "R" : "B");

    for (int i = 0; i < 32; ++i) {
        const ChessPieces& piece = m_ChessPieces[i];
        parts.append(QStringLiteral("%1:%2:%3:%4")
                         .arg(i)
                         .arg(piece.m_bDead ? 1 : 0)
                         .arg(piece.m_nRow)
                         .arg(piece.m_nCol));
    }

    return parts.join('|');
}

void ChessBoard::resetRuleTracking()
{
    m_positionHistory.clear();
    m_moveTraces.clear();
    m_positionHistory.append(boardSignature());
}

QSet<int> ChessBoard::chaseTargetsForPiece(int moveid) const
{
    QSet<int> targets;
    if (moveid < 0 || moveid >= 32 || m_ChessPieces[moveid].m_bDead)
        return targets;

    const bool targetRed = !m_ChessPieces[moveid].m_bRed;
    for (int targetId = 0; targetId < 32; ++targetId) {
        if (m_ChessPieces[targetId].m_bDead || m_ChessPieces[targetId].m_bRed != targetRed)
            continue;
        if (m_ChessPieces[targetId].m_emType == ChessPieces::JIANG)
            continue;
        if (const_cast<ChessBoard*>(this)->canMove(moveid, targetId, m_ChessPieces[targetId].m_nRow, m_ChessPieces[targetId].m_nCol)) {
            targets.insert(targetId);
        }
    }

    return targets;
}

void ChessBoard::recordBoardState(int moveid)
{
    MoveTrace trace;
    trace.moverRed = isRed(moveid);
    trace.givesCheck = isSideInCheck(m_bIsRed);
    trace.chaseTargets = chaseTargetsForPiece(moveid);

    m_moveTraces.append(trace);
    m_positionHistory.append(boardSignature());
}

bool ChessBoard::sideAllChecks(const QVector<MoveTrace>& traces, bool redSide) const
{
    bool found = false;
    for (const MoveTrace& trace : traces) {
        if (trace.moverRed != redSide)
            continue;
        found = true;
        if (!trace.givesCheck)
            return false;
    }
    return found;
}

bool ChessBoard::sideAllChases(const QVector<MoveTrace>& traces, bool redSide) const
{
    bool found = false;
    QSet<int> commonTargets;

    for (const MoveTrace& trace : traces) {
        if (trace.moverRed != redSide)
            continue;

        if (trace.givesCheck || trace.chaseTargets.isEmpty())
            return false;

        if (!found) {
            commonTargets = trace.chaseTargets;
            found = true;
        } else {
            commonTargets.intersect(trace.chaseTargets);
        }

        if (commonTargets.isEmpty())
            return false;
    }

    return found && !commonTargets.isEmpty();
}

bool ChessBoard::detectRepeatedOutcome(GameResult& result, QString& message)
{
    if (m_positionHistory.size() < 3)
        return false;

    const QString current = m_positionHistory.last();
    QVector<int> occurrences;
    for (int i = 0; i < m_positionHistory.size(); ++i) {
        if (m_positionHistory[i] == current)
            occurrences.append(i);
    }

    if (occurrences.size() < 3)
        return false;

    const int first = occurrences[occurrences.size() - 3];
    const int second = occurrences[occurrences.size() - 2];
    const int third = occurrences[occurrences.size() - 1];
    const int gap1 = second - first;
    const int gap2 = third - second;
    if (gap1 <= 0 || gap1 != gap2)
        return false;

    QVector<MoveTrace> cycle;
    for (int moveIndex = second; moveIndex < third; ++moveIndex) {
        if (moveIndex >= 0 && moveIndex < m_moveTraces.size())
            cycle.append(m_moveTraces[moveIndex]);
    }

    const bool redLongCheck = sideAllChecks(cycle, true);
    const bool blackLongCheck = sideAllChecks(cycle, false);
    if (redLongCheck && !blackLongCheck) {
        result = GameResult::BlackWin;
        message = QStringLiteral("本局结束，红方长将判负，黑方胜利.");
        return true;
    }
    if (blackLongCheck && !redLongCheck) {
        result = GameResult::RedWin;
        message = QStringLiteral("本局结束，黑方长将判负，红方胜利.");
        return true;
    }

    const bool redLongChase = sideAllChases(cycle, true);
    const bool blackLongChase = sideAllChases(cycle, false);
    if (redLongChase && !blackLongChase) {
        result = GameResult::BlackWin;
        message = QStringLiteral("本局结束，红方长捉判负，黑方胜利.");
        return true;
    }
    if (blackLongChase && !redLongChase) {
        result = GameResult::RedWin;
        message = QStringLiteral("本局结束，黑方长捉判负，红方胜利.");
        return true;
    }

    result = GameResult::Draw;
    message = QStringLiteral("本局结束，重复局面判和.");
    return true;
}

ChessBoard::GameResult ChessBoard::detectGameResult(bool includeBoardState, QString& message)
{
    clearLastGameResult();
    GameResult result = GameResult::None;

    if ((m_ChessPieces[4].m_bDead && !m_ChessPieces[20].m_bDead) || errCnt[0] >= maxErrCnt) {
        result = GameResult::RedWin;
        message = errCnt[0] >= maxErrCnt
            ? QStringLiteral("本局结束，黑方累计 3 次违规，红方胜利.")
            : QStringLiteral("本局结束，红方胜利.");
    } else if ((!m_ChessPieces[4].m_bDead && m_ChessPieces[20].m_bDead) || errCnt[1] >= maxErrCnt) {
        result = GameResult::BlackWin;
        message = errCnt[1] >= maxErrCnt
            ? QStringLiteral("本局结束，红方累计 3 次违规，黑方胜利.")
            : QStringLiteral("本局结束，黑方胜利.");
    } else if (includeBoardState) {
        if (!hasAnyLegalMove(m_bIsRed)) {
            const bool checked = isSideInCheck(m_bIsRed);
            const QString loser = m_bIsRed ? QStringLiteral("红") : QStringLiteral("黑");
            const QString winner = m_bIsRed ? QStringLiteral("黑") : QStringLiteral("红");
            result = m_bIsRed ? GameResult::BlackWin : GameResult::RedWin;
            message = checked
                ? QStringLiteral("本局结束，%1方被将死，%2方胜利.").arg(loser, winner)
                : QStringLiteral("本局结束，%1方困毙，%2方胜利.").arg(loser, winner);
        } else if (detectRepeatedOutcome(result, message)) {
            // result/message already assigned
        }
    }

    m_lastGameResult = result;
    m_lastGameMessage = message;
    return result;
}

//鼠标按下事件
//void ChessBoard::mousePressEvent(QMouseEvent *ev)
//{
//    //只响应鼠标左键的单击操作 防止游戏结束重复弹框
//    if(ev->button() != Qt::LeftButton || ev->type() != QEvent::Type::MouseButtonPress)
//        return;

//    QPoint pt = ev->pos();
//    pt = getRealPoint(pt);
//    //将pt转化成棋盘的像行列值
//    //判断这个行列值上面有没有棋子
//    int row, col;

//    //点击棋盘外面就不做处理
//    if(!isChecked(pt, row, col))
//        return;

//    if(m_bIsOver)
//    {
//        QMessageBox message(QMessageBox::Information, "提示", "本局已结束，请重新开始.");
//        message.setIconPixmap(QPixmap(":/images/win.jpg"));
//        message.setFont(QFont("FangSong",16,QFont::Bold));
//        message.exec();
//        return;
//    }

//    //判断是哪一个棋子被选中，根据ID（这里的局部i）来记录下来
//    int i;
//    m_nCheckedID = -1;

//    for(i = 0; i <= 31; i++)
//    {
//        if(m_ChessPieces[i].m_nRow == row && m_ChessPieces[i].m_nCol == col && !m_ChessPieces[i].m_bDead)
//            break;
//    }

//    if(0<=i && i<32)
//        m_nCheckedID = i;  //选中的棋子的ID

//    bool newbIsRed = m_bIsRed;
//    clickPieces(m_nCheckedID, row, col);

//    // 刚执棋落子完成，出现对将
//    if (hongMenFeast() && m_nSelectID == -1 && newbIsRed != m_bIsRed)
//    {
//        winMessageBox("提示", "可将军，直接取胜");
//        // TODO: 可将军，直接提示直接取胜的音效
//    }

//    update();
//    whoWin();
//}

//void ChessBoard::clickPieces(int checkedID, int& row, int& col)
//{
//    m_nCheckedID = checkedID;

//    if(m_nSelectID == -1) //选中棋子
//    {
//       // whoPlay(m_nCheckedID);

//        if(m_nCheckedID != -1)
//        {
//            if(m_bIsRed == m_ChessPieces[m_nCheckedID].m_bRed)
//            {
//                m_nSelectID = m_nCheckedID;
//                m_Chessvoice.voiceSelect();   //选棋音效
//            }
//        }
//    }
//    else//走棋子
//    {
//        if(canMove(m_nSelectID, m_nCheckedID, row, col ))
//        {
//            //m_nSelectID为第一次点击选中的棋子，
//            //m_nCheckedID为第二次点击||被杀的棋子ID，准备选中棋子下子的地方
//            m_ChessPieces[m_nSelectID].m_nRow = row;
//            m_ChessPieces[m_nSelectID].m_nCol = col;
//            if(m_nCheckedID != -1)
//            {
//                m_ChessPieces[m_nCheckedID].m_bDead = true;
//                m_Chessvoice.voiceEat();  //吃子音效
//            }
//            m_Chessvoice.voiceMove(); //移动音效

//            m_nSelectID = -1;
//            m_bIsRed = !m_bIsRed;
//        }
//    }
//}


//总的移动规则，选中准备下的棋子，被杀的棋子， 准备移动到的目的行列值
//bool ChessBoard::canMove(int moveId, int killId, int row, int col)
//{
//    //1.确定是选择其它棋子还是走棋
//    //2.是否需要使用到canMoveXXX()来做限制
//    //3.罗列出所有情况，和需要的得到的结果值 ==>  然后进行中间的逻辑层判断※不要受到别人的代码框架的束缚※

//        if(isRed(moveId) == m_ChessPieces[killId].m_bRed)  //选择其它棋子，返回false
//        {
//            if(killId == -1)  //其中有一个特殊情况，黑+m_ChessPieces[-1].m_bRed ==> 也需要判断能否
//            {
//                switch (m_ChessPieces[moveId].m_emType)
//                {
//                case ChessPieces::JIANG:
//                    return canMoveJIANG(moveId, killId, row, col);
//                case ChessPieces::SHI:
//                    return canMoveSHI(moveId, killId, row, col);
//                case ChessPieces::XIANG:
//                    return canMoveXIANG(moveId, killId, row, col);
//                case ChessPieces::MA:
//                    return canMoveMA(moveId, killId, row, col);
//                case ChessPieces::CHE:
//                    return canMoveCHE(moveId, killId, row, col);
//                case ChessPieces::PAO:
//                    return canMovePAO(moveId, killId, row, col);
//                case ChessPieces::BING:
//                    return canMoveBING(moveId, killId, row, col);
//                }
//            }
//            m_nSelectID = killId;

//            return false;
//        }
//        else  //选择其走棋，返回true
//        {
//            switch (m_ChessPieces[moveId].m_emType)
//            {
//            case ChessPieces::JIANG:
//                return canMoveJIANG(moveId, killId, row, col);
//            case ChessPieces::SHI:
//                return canMoveSHI(moveId, killId, row, col);
//            case ChessPieces::XIANG:
//                return canMoveXIANG(moveId, killId, row, col);
//            case ChessPieces::MA:
//                return canMoveMA(moveId, killId, row, col);
//            case ChessPieces::CHE:
//                return canMoveCHE(moveId, killId, row, col);
//            case ChessPieces::PAO:
//                return canMovePAO(moveId, killId, row, col);
//            case ChessPieces::BING:
//                return canMoveBING(moveId, killId, row, col);
//            }

//            return true;

//        }
//}

//总的移动规则
bool ChessBoard::canMove(int moveId, int killId, int row, int col)
{
    //选棋id和吃棋id同色，则选择其它棋子并返回
    if(row<0 || row>9 || col<0 || col>8) return false;

    if(sameColor(moveId,killId))
    {
        //换选棋子
        m_nSelectID=killId;
        update();
        return false;
    }

    switch (m_ChessPieces[moveId].m_emType)
    {
    case ChessPieces::JIANG:
        return canMoveJIANG(moveId, killId, row, col);

    case ChessPieces::SHI:
        return canMoveSHI(moveId, killId, row, col);

    case ChessPieces::XIANG:
        return canMoveXIANG(moveId, killId, row, col);

    case ChessPieces::MA:
        return canMoveMA(moveId, killId, row, col);

    case ChessPieces::CHE:
        return canMoveCHE(moveId, killId, row, col);

    case ChessPieces::PAO:
        return canMovePAO(moveId, killId, row, col);

    case ChessPieces::BING:
        return canMoveBING(moveId, killId, row, col);

    default: break;
    }

    return false;
}

bool ChessBoard::canMoveJIANG(int moveId, int killId, int row, int col)
{
    //对将的情况
    if (killId != -1 && m_ChessPieces[killId].m_emType == m_ChessPieces->JIANG)
        return canMoveCHE(moveId, killId, row, col );

    if(isRed(moveId)) //红 将
    {
        if(row < 7 || col < 3 || col > 5) return false;
    }
    else  //黑 将
    {
        if(row > 2 || col < 3 || col > 5) return false;
    }

    int d=relation(m_ChessPieces[moveId].m_nRow, m_ChessPieces[moveId].m_nCol, row, col);
    if(d == 1 || d == 10)
        return true;

    return false;
}

bool ChessBoard::canMoveSHI(int moveId, int killId, int row, int col)
{
    Q_UNUSED(killId);
    if(isRed(moveId)) //红 士
    {
        if(row < 7 || col < 3 || col > 5) return false;
    }
    else  //黑 士
    {
        if(row > 2 || col < 3 || col > 5) return false;
    }

    int d=relation(m_ChessPieces[moveId].m_nRow, m_ChessPieces[moveId].m_nCol, row, col);
    if(d == 11)
        return true;

    return false;
}

bool ChessBoard::canMoveXIANG(int moveId, int killId, int row, int col)
{
    Q_UNUSED(killId);
    int d=relation(m_ChessPieces[moveId].m_nRow, m_ChessPieces[moveId].m_nCol, row, col);
    if(d!= 22)
        return false;

    int row_eye= (m_ChessPieces[moveId].m_nRow+ row)/ 2;
    int col_eye= (m_ChessPieces[moveId].m_nCol+ col)/ 2;

    //堵象眼
    if(getStoneId(row_eye,col_eye)!= -1)
        return false;

    //象不可过河
    if(isRed(moveId))   //红
    {
        if(row< 4)
            return false;
    }
    else    //黑
    {
        if(row> 5)
            return false;
    }

    return true;
}

bool ChessBoard::canMoveMA(int moveId, int killId, int row, int col)
{
    Q_UNUSED(killId);
    int d=relation(m_ChessPieces[moveId].m_nRow, m_ChessPieces[moveId].m_nCol, row, col);
    if(d!=12 && d!=21)
        return false;

    //蹩马脚
    if(d==12)
    {
        if(getStoneId(m_ChessPieces[moveId].m_nRow, (m_ChessPieces[moveId].m_nCol+ col) /2) != -1)
            return false;
    }
    else
    {
        if(getStoneId((m_ChessPieces[moveId].m_nRow+ row) /2 ,m_ChessPieces[moveId].m_nCol) != -1)
            return false;
    }

    return true;
}

bool ChessBoard::canMoveCHE(int moveId, int killId, int row, int col)
{
    Q_UNUSED(killId);
    int ret = getStoneCountAtLine(m_ChessPieces[moveId].m_nRow, m_ChessPieces[moveId].m_nCol, row, col);
    if(ret == 0)
        return true;

    return false;
}

bool ChessBoard::canMovePAO(int moveId, int killId, int row, int col)
{
    int ret = getStoneCountAtLine(row, col, m_ChessPieces[moveId].m_nRow, m_ChessPieces[moveId].m_nCol);
    if(killId != -1)
    {
        if(ret == 1)
            return true;
    }
    else
    {
        if(ret == 0)
            return true;
    }
    return false;
}

bool ChessBoard::canMoveBING(int moveId, int killId, int row, int col)
{
    Q_UNUSED(killId);
    int d=relation(m_ChessPieces[moveId].m_nRow, m_ChessPieces[moveId].m_nCol, row, col);
    if(d!= 1 && d!= 10)
        return false;

    if(isRed(moveId))   //红
    {
        //兵卒不可后退
        if(row> m_ChessPieces[moveId].m_nRow)
            return false;

        //兵卒没过河不可横着走
        if(m_ChessPieces[moveId].m_nRow>= 5 && m_ChessPieces[moveId].m_nRow== row)
            return false;
    }
    else    //黑
    {
        if(row< m_ChessPieces[moveId].m_nRow)
            return false;
        if(m_ChessPieces[moveId].m_nRow<= 4 && m_ChessPieces[moveId].m_nRow== row)
            return false;
    }

    return true;
}
// 被选中的棋子颜色要和当前棋方的颜色相同
bool ChessBoard:: canSelect(int id, bool isRedSend)
{
    return id>=0 && id<32 && !m_ChessPieces[id].m_bDead && isRedSend==m_bIsRed && m_bIsRed== m_ChessPieces[id].m_bRed;
}
bool ChessBoard:: canSelect(int id)
{
    return m_bIsRed== m_ChessPieces[id].m_bRed;
}

void ChessBoard::mouseReleaseEvent(QMouseEvent *ev)
{
    if (ev->button() != Qt::LeftButton || m_bIsOver== true) { // 排除鼠标右键点击 游戏已结束则直接返回
        return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPointF globalPos = ev->globalPosition();
#else
    const QPointF globalPos = ev->globalPos();
#endif
    const QPointF windowTopLeft = mapToGlobal(QPoint(0, 0));
    const QPointF mousePos = globalPos - windowTopLeft;  // 统一转换为 ChessBoard 坐标系

    QPointF pt = getRealPoint(mousePos);  // 转换为虚拟坐标
    click(pt);
}

void ChessBoard::click(QPointF pt)
{
    // 看有没有点中象棋
    // 将pt转化成象棋的行列值
    // 判断这个行列值上面有没有棋子
    int row, col;
    bool bClicked = isChecked(pt, row, col); // 是否点击在有效区域内，如果是，计算出对应的row和col
    if (!bClicked) {
        return;
    }

    int id = getStoneId(row, col); // 遍历32个象棋，找出位置为(row, col)的未死棋子，没找到则返回-1
    // 应该是NetworkGame::clickPieces，因为这是个虚函数，父类可以调用子类的方法
    clickPieces(id, row, col);

}

void ChessBoard::clickPieces(int id, int &row, int &col)
{
    if (this->m_nSelectID == -1) { // 如果点中的棋子之前未被选中
        trySelectStone(id); // 之前没选中棋子，则选择棋子
    }
    else {
        tryMoveStone(id, row, col); // 如果之前已经选中棋子，则移动棋子
    }

}

void ChessBoard::trySelectStone(int id)
{
    if (id == -1) {
        return;
    }

    if (!canSelect(id)) {
        return;
    }

    m_nSelectID = id;
    update();
    m_Chessvoice.voiceSelect();
}
// 如果目标位置有棋子，则killid为对于棋子id；否则为-1
void ChessBoard::tryMoveStone(int killid, int row, int col)
{
    if (killid != -1 && sameColor(killid, m_nSelectID)) {
        trySelectStone(killid);
        return;
    }

    bool ret = canMove(m_nSelectID, killid, row, col);
    if (ret && wouldCauseSelfCheck(m_nSelectID, killid, row, col)) {
        ret = false;
    }
    if (ret) {
        doMoveStone(m_nSelectID, killid, row, col);
        m_nSelectID = -1;
        update();
    }
}

// 如果目标位置有棋子，则killid为对于棋子id；否则为-1
bool ChessBoard::tryMoveStone(int nCheckedID, int killid, int row, int col, bool isRed)
{
    if (!canSelect(nCheckedID, isRed) ||  killid != -1 && sameColor(killid, nCheckedID)) {
        return false;
    }

    if(!canMove(nCheckedID, killid, row, col)) return false;
    if(wouldCauseSelfCheck(nCheckedID, killid, row, col)) return false;
    doMoveStone(nCheckedID, killid, row, col, false);
    update();
    return true;
}

void ChessBoard::doMoveStone(int moveid, int killid, int row, int col, bool presentResult)
{
    autoStartTimerIfNeeded();
    saveStep(moveid, killid, row, col, m_ChessSteps);

    killStone(killid);
    moveStone(moveid, row, col);
    recordBoardState(moveid);
    QString message;
    detectGameResult(true, message);
    if (presentResult) {
        presentLastGameResult();
    }

    if(killid== -1)
        m_Chessvoice.voiceMove(); //移动音效
    else
        m_Chessvoice.voiceEat(); //吃子音效

    if(isGeneral())
        m_Chessvoice.voiceGeneral();    //将军音效
}

void ChessBoard::saveStep(int moveid, int killid, int row, int col, QVector<ChessStep*>& steps)
{
    ChessStep* step = new ChessStep;
    step->m_nColFrom = m_ChessPieces[moveid].m_nCol;
    step->m_nnColTo = col;
    step->m_nRowFrom = m_ChessPieces[moveid].m_nRow;
    step->m_nRowTo = row;
    step->m_nMoveID = moveid;
    step->m_nKillID = killid;

    steps.append(step);
    textStepRecord= textStep(moveid, row, col);
}

QString ChessBoard::textStep(int id, int row, int col)
{
    int rowFrom= m_ChessPieces[id].m_nRow;
    int rowTo= row;
    int colFrom= m_ChessPieces[id].m_nCol;
    int colTo= col;

    QString temp="";
    QString name=m_ChessPieces[id].getnName(m_ChessPieces[id].m_bRed);
    QString textCol= m_ChessPieces[id].getColText(colFrom);
    QString textRow= m_ChessPieces[id].getRowText(rowTo);
    temp.append(name).append(textCol).append(textRow);

    //兵炮车将
    if(m_ChessPieces[id].m_emType==6 || m_ChessPieces[id].m_emType==5 || m_ChessPieces[id].m_emType==4 || m_ChessPieces[id].m_emType==0)
    {
        //行相等
        if(rowFrom== rowTo)
        {
            temp.append(m_ChessPieces[id].getColText(colTo));
            return temp;
        }
        //移动的格数
        temp.append(m_ChessPieces[id].getMoveText(rowFrom, rowTo));
    }
    else    //马相士
    {
        //移动后所在列
        temp.append(m_ChessPieces[id].getColText(colTo));
    }
    return temp;

}

void ChessBoard::backOne()
{
    if (this->m_ChessSteps.size() == 0 || m_bIsOver) {
        return;
    }

    ChessStep* step = this->m_ChessSteps.last();
    m_ChessSteps.removeLast();
    back(step);

    update();
    delete step;
    m_Chessvoice.voiceBack();
    clearLastGameResult();

    if (!m_moveTraces.isEmpty()) {
        m_moveTraces.removeLast();
    }
    if (m_positionHistory.size() > 1) {
        m_positionHistory.removeLast();
    }

    // 悔棋到第0步时停止计时器
    if (m_ChessSteps.size() == 0 && m_bTimerAutoStarted) {
        pauseGameTimer();
        m_bTimerAutoStarted = false;
        ui->pushButton_start->setText("开始");
    }
}

void ChessBoard::back(ChessStep* step)
{
    reliveStone(step->m_nKillID);
    moveStone(step->m_nMoveID, step->m_nRowFrom, step->m_nColFrom);
}

void ChessBoard::back()
{
    backOne();
}

//刷新时间
void ChessBoard::updateTime()
{
    *m_timeRecord = m_timeRecord->addSecs(1);
    ui->lcdNumber->display(m_timeRecord->toString("hh:mm:ss"));
}

void ChessBoard::on_pushButton_start_clicked()
{
    if(!m_bIsStart) //尚未开始 开始计时
    {
        startGameTimer();
        m_bTimerAutoStarted = true;
    }
    else //已经开始，暂停
    {
        pauseGameTimer();
    }
}

void ChessBoard::on_pushButton_reset_clicked()
{
    pauseGameTimer();    //计时器停止
    m_timeRecord->setHMS(0,0,0); //时间设为0
    ui->lcdNumber->display(m_timeRecord->toString("hh:mm:ss")); //显示00:00:00
    ui->pushButton_start->setText("开始");
    ui->pushButton_start->setEnabled(true);
    m_bTimerAutoStarted = false;
}

void ChessBoard::on_pushButton_about_clicked()
{
    m_pAbout->setWindowTitle("关于作者");
    m_pAbout->show();
}

void ChessBoard::on_pushButton_restart_clicked()
{
    init();
    updateViolationDisplay();
    on_pushButton_reset_clicked();
    update();
}

void ChessBoard::on_pushButton_back_clicked()
{
    back();
    update();
}

void ChessBoard::on_pushButton_showStep_clicked()
{
    m_bIsShowStep=!m_bIsShowStep;
    update();
}

void ChessBoard::on_pushButton_toMenu_clicked()
{
    emit this->toMenu();
}
