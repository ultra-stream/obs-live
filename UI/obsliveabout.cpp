#include "obsliveabout.hpp"

#include "obs-app.hpp"

OBSLiveAbout::OBSLiveAbout(QWidget *parent) : QDialog(parent), ui(new Ui::OBSLiveAbout)
{
	ui->setupUi(this);

	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	QString appVersion = QString(App()->GetVersionString().c_str());

#ifndef OBS_LIVE_PRODUCTION_BUILD
	appVersion.prepend("(Dev)");
#endif

	ui->aboutLabel->setText(ui->aboutLabel->text() + QString("<br><br>version ") + appVersion);
}
