/****************************************************************************
 * 简单 Qt 界面示例：深色主题 + 多按钮
 ****************************************************************************/

#include <QApplication>
#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QMessageBox>
#include <QPalette>
#include <QLabel>

int main(int argc, char *argv[])
{
	QApplication app(argc, argv);
	app.setApplicationName("Qt 简单示例");

	/* 深色主题：深灰背景 + 浅色文字，便于看清鼠标 */
	app.setStyleSheet(
		"QMainWindow, QWidget { background-color: #2d2d2d; }"
		"QLabel { color: #e0e0e0; font-size: 14px; }"
		"QPushButton {"
		"  background-color: #404040; color: #e0e0e0; border: 1px solid #505050;"
		"  padding: 10px 20px; font-size: 14px; min-height: 20px;"
		"  border-radius: 4px;"
		"}"
		"QPushButton:hover { background-color: #505050; border-color: #606060; }"
		"QPushButton:pressed { background-color: #303030; }"
	);

	QMainWindow window;
	window.setWindowTitle("Qt 简单界面示例");
	window.setMinimumSize(400, 280);

	QWidget *central = new QWidget(&window);
	QVBoxLayout *layout = new QVBoxLayout(central);
	layout->setSpacing(12);
	layout->setContentsMargins(20, 20, 20, 20);

	QLabel *title = new QLabel("选择操作：", central);
	layout->addWidget(title);

	QPushButton *btn1 = new QPushButton("按钮 1 - 提示", central);
	QPushButton *btn2 = new QPushButton("按钮 2 - 问候", central);
	QPushButton *btn3 = new QPushButton("按钮 3 - 关于", central);
	QPushButton *btn4 = new QPushButton("按钮 4 - 关闭", central);

	btn1->setMinimumHeight(44);
	btn2->setMinimumHeight(44);
	btn3->setMinimumHeight(44);
	btn4->setMinimumHeight(44);
	layout->addWidget(btn1);
	layout->addWidget(btn2);
	layout->addWidget(btn3);
	layout->addWidget(btn4);

	QObject::connect(btn1, &QPushButton::clicked, [&window]() {
		QMessageBox::information(&window, "提示", "按钮 1 被点击了！");
	});
	QObject::connect(btn2, &QPushButton::clicked, [&window]() {
		QMessageBox::information(&window, "问候", "你好，X5 开发板！");
	});
	QObject::connect(btn3, &QPushButton::clicked, [&window]() {
		QMessageBox::about(&window, "关于", "Qt 简单示例 - 深色主题");
	});
	QObject::connect(btn4, &QPushButton::clicked, [&window]() {
		window.close();
	});

	layout->addStretch();
	window.setCentralWidget(central);
	window.show();

	return app.exec();
}
