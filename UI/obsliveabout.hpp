#pragma once

#include <memory>
#include <QDialog>

#include "ui_OBSLiveAbout.h"

class OBSLiveAbout : public QDialog {
	Q_OBJECT

public:
	explicit OBSLiveAbout(QWidget *parent = 0);

	std::unique_ptr<Ui::OBSLiveAbout> ui;
};
