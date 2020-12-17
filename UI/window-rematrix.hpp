#pragma once

#include <QDialog>
#include <QList>
#include <QDoubleSpinBox>
#include <QComboBox>

#include <obs.hpp>

#include "ui_Rematrix.h"

class QAbstractButton;
class OBSBasic;

class RematrixDialog : public QDialog {
	Q_OBJECT

	OBSBasic *m_main;
	Ui::Rematrix* m_ui;

	OBSSource m_source;
	OBSSource m_filter;

	QList<QDoubleSpinBox*> m_spinBoxes;
	QList<QComboBox*> m_comboBoxes;

	bool m_sliderMoved = false;
	bool m_spinBoxClicked = false;

	virtual void closeEvent(QCloseEvent *event) override;
	virtual void reject() override;

	void fillComboBoxWithValues(QComboBox* control);

public:
	explicit RematrixDialog(QWidget *parent, OBSSource source, OBSBasic *main);
	virtual ~RematrixDialog() override;

	static bool registerSource();

private slots:
	void on_buttonBox_clicked(QAbstractButton *button);
};
