
#ifndef DIALOG_PRINT_H_
#define DIALOG_PRINT_H_

#include "Dialog.h"
#include "ui_DialogPrint.h"

class DialogPrint : public Dialog {
	Q_OBJECT
public:
	DialogPrint(const QString &PrintFileName, const QString &jobName, QWidget *parent = 0, Qt::WindowFlags f = 0);
    virtual ~DialogPrint() = default;

private Q_SLOTS:
	void on_spinCopies_valueChanged(int n);
	void on_editQueue_textChanged(const QString &text);
	void on_editHost_textChanged(const QString &text);
	void on_editCommand_textEdited(const QString &text);
	void on_buttonPrint_clicked();

private:
	void updatePrintCmd();

public:
    static void LoadPrintPreferencesEx(bool lookForFlpr);

private:	
	static bool flprPresent();
	static void getFlprQueueDefault(char *defqueue);
	static void getFlprHostDefault(char *defhost);
	static void getLprQueueDefault(char *defqueue);
	static bool fileInPath(const char *filename, uint16_t mode_flags);
	static bool foundEnv(const char *EnvVarName, char *result);
	static bool foundTag(const char *tagfilename, const char *tagname, char *result);
	static bool fileInDir(const char *filename, const char *dirpath, uint16_t mode_flags);

public:
	Ui::DialogPrint ui;
	static bool PreferencesLoaded;
	QString PrintFileName_;
	QString PrintJobName_;
};

#endif


