#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QDialogButtonBox>
#include <QAbstractButton>

#include <media-io/audio-math.h>

#include "window-rematrix.hpp"

#include "window-basic-main.hpp"
#include "obs-app.hpp"

#include "qt-wrappers.hpp"



namespace {
	const QString COMBOBOX_STYLESHEET = "QComboBox {	border: 1px solid lightgrey;	min-width: 6em;	min-height: 29px;	background: white;}QComboBox::drop-down:!editable {	 background: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1,								 stop: 0 #E1E1E1, stop: 0.4 #DDDDDD,								 stop: 0.5 #D8D8D8, stop: 1.0 #D3D3D3);}/* QComboBox gets the \"on\" state when the popup is open */ QComboBox::drop-down:!editable:on {	background: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1,								stop: 0 #D3D3D3, stop: 0.4 #D8D8D8,								stop: 0.5 #DDDDDD, stop: 1.0 #E1E1E1);}QComboBox:on { /* shift the text when the popup opens */	padding-top: 3px;	padding-left: 4px;}QComboBox::down-arrow {	image: url(:/settings/images/settings/combobox-arrows.png);}";
	const double DB_MIN_VALUE = -30.f;
	const double DB_MAX_VALUE = 30.f;
	const double DB_DEFAULT_VALUE = 0.f;
	const double SLIDER_SCALE = 10.f;
	const char* SOURCE_ID = "SLRematrix";
	const char* SOURCE_NAME = "SLRematrixDefault";

#ifndef MAX_AUDIO_SIZE
#ifndef AUDIO_OUTPUT_FRAMES
#define	AUDIO_OUTPUT_FRAMES 1024
#endif
#define	MAX_AUDIO_SIZE (AUDIO_OUTPUT_FRAMES * sizeof(float))
#endif // !MAX_AUDIO_SIZE

	struct rematrix_data {
		obs_source_t *context;
		size_t channels;
		//store the routing information
		std::atomic<long> route[MAX_AV_PLANES];
		std::atomic<float> gain[MAX_AV_PLANES];
		//store a temporary buffer
		uint8_t *tmpbuffer[MAX_AV_PLANES];
	};

	/*****************************************************************************/
	static const char *rematrix_name(void *unused) {
		UNUSED_PARAMETER(unused);
		return SOURCE_NAME;
	}

	long long get_obs_output_channels() {
		// get channel number from output speaker layout set by obs
		struct obs_audio_info aoi;
		obs_get_audio_info(&aoi);
		long long recorded_channels = get_audio_channels(aoi.speakers);
		return recorded_channels;
	}

	/*****************************************************************************/
	static void rematrix_destroy(void *data) {
		struct rematrix_data *rematrix = (struct rematrix_data *)data;

		for (size_t i = 0; i < rematrix->channels; i++) {
			if (rematrix->tmpbuffer[i])
				bfree(rematrix->tmpbuffer[i]);
		}

		bfree(rematrix);
	}

	/*****************************************************************************/
	static void rematrix_update(void *data, obs_data_t *settings) {
		struct rematrix_data *rematrix = (struct rematrix_data *)data;

		rematrix->channels = audio_output_get_channels(obs_get_audio());

		bool route_changed = false;
		bool gain_changed = false;
		long route[MAX_AV_PLANES];
		float gain[MAX_AV_PLANES];

		//make enough space for c strings
		int pad_digits = (int)floor(log10(abs(MAX_AV_PLANES))) + 1;

		//template out the route format
		const char* route_name_format = "route %i";
		size_t route_len = strlen(route_name_format) + pad_digits;
		char* route_name = (char *)calloc(route_len, sizeof(char));

		//template out the gain format
		const char* gain_name_format = "gain %i";
		size_t gain_len = strlen(gain_name_format) + pad_digits;
		char* gain_name = (char *)calloc(gain_len, sizeof(char));

		//copy the routing over from the settings
		for (long long i = 0; i < MAX_AV_PLANES; i++) {
			sprintf(route_name, route_name_format, i);
			sprintf(gain_name, gain_name_format, i);

			route[i] = (int)obs_data_get_int(settings, route_name);
			gain[i] = (float)obs_data_get_double(settings, gain_name);

			gain[i] = db_to_mul(gain[i]);

			long other_route = rematrix->route[i].load();
			float other_gain = rematrix->gain[i].load();

			if (!rematrix->route[i].compare_exchange_strong(other_route, route[i]))
				route_changed = true;
			if (!rematrix->gain[i].compare_exchange_strong(other_gain, gain[i]))
				gain_changed = true;
		}

		//don't memory leak
		free(route_name);
		free(gain_name);
	}

	static void *rematrix_create(obs_data_t *settings, obs_source_t *filter) {
		//struct rematrix_data *rematrix = bzalloc(sizeof(*rematrix));
		struct rematrix_data *rematrix = (struct rematrix_data *) bzalloc(sizeof(*rematrix));
		rematrix->context = filter;
		rematrix_update(rematrix, settings);

		for (size_t i = 0; i < rematrix->channels; i++) {
			rematrix->tmpbuffer[i] = (uint8_t *)bzalloc(MAX_AUDIO_SIZE);
		}

		return rematrix;
	}

	static struct obs_audio_data *rematrix_filter_audio(void *data,
														struct obs_audio_data *audio) {

		struct rematrix_data *rematrix = (struct rematrix_data *)data;
		const size_t channels = rematrix->channels;
		float **fmatrixed_data = (float**)rematrix->tmpbuffer;
		float **fdata = (float**)audio->data;
		size_t ch_buffer = (audio->frames * sizeof(float));
		float route[MAX_AV_PLANES];
		float gain[MAX_AV_PLANES];

		//prevent race condition
		for (size_t c = 0; c < channels; c++) {
			route[c] = rematrix->route[c].load();
			gain[c] = rematrix->gain[c].load();
		}

		uint32_t frames = audio->frames;
		size_t copy_size = 0;
		size_t unprocessed_samples = 0;
		//consume AUDIO_OUTPUT_FRAMES or less # of frames
		for (size_t chunk = 0; chunk < frames; chunk += AUDIO_OUTPUT_FRAMES) {
			//calculate the size of the data we're about to try to copy
			if (frames - chunk < AUDIO_OUTPUT_FRAMES)
				unprocessed_samples = frames - chunk;
			else
				unprocessed_samples = AUDIO_OUTPUT_FRAMES;
			copy_size = unprocessed_samples * sizeof(float);

			//copy data to temporary buffer
			for (size_t c = 0; c < channels; c++) {
				//valid route copy data to temporary buffer
				if (fdata[c] && route[c] >= 0 && route[c] < channels)
					memcpy(fmatrixed_data[c],
						   &fdata[(int)route[c]][chunk],
						   copy_size);
					//not a valid route, mute
				else
					memset(fmatrixed_data[c], 0, MAX_AUDIO_SIZE);
			}

			//move data into place and process gain
			for (size_t c = 0; c < channels; c++) {
				if (!fdata[c])
					continue;
				for (size_t s = 0; s < unprocessed_samples; s++)
					fdata[c][chunk + s] = fmatrixed_data[c][s] * gain[c];
			}
			//move to next chunk of unprocessed data
		}
		return audio;
	}

/*****************************************************************************/
	static void rematrix_defaults(obs_data_t *settings)
	{
		//make enough space for c strings
		int pad_digits = (int)floor(log10(abs(MAX_AV_PLANES))) + 1;

		//template out the route format
		const char* route_name_format = "route %i";
		size_t route_len = strlen(route_name_format) + pad_digits;
		char* route_name = (char *)calloc(route_len, sizeof(char));

		//template out the gain format
		const char* gain_name_format = "gain %i";
		size_t gain_len = strlen(gain_name_format) + pad_digits;
		char* gain_name = (char *)calloc(gain_len, sizeof(char));

		//default is no routing (ordered) -1 or any out of bounds is mute*
		for (long long i = 0; i < MAX_AV_PLANES; i++) {
			sprintf(route_name, route_name_format, i);
			sprintf(gain_name, gain_name_format, i);

			obs_data_set_default_int(settings, route_name, i);
			obs_data_set_default_double(settings, gain_name, 0.0);
		}

		obs_data_set_default_string(settings, "profile_name", "Default");

		//don't memory leak
		free(gain_name);
		free(route_name);
	}

	/*****************************************************************************/
	static bool fill_out_channels(obs_properties_t *props, obs_property_t *list,
								  obs_data_t *settings) {

		obs_property_list_clear(list);
		obs_property_list_add_int(list, "mute", -1);
		long long channels = get_obs_output_channels();

		//make enough space for c strings
		int pad_digits = (int)floor(log10(abs(MAX_AV_PLANES))) + 1;

		//template out the format for the json
		const char* route_obs_format = "in.ch.%i";
		size_t route_obs_len = strlen(route_obs_format) + pad_digits;
		char* route_obs = (char *)calloc(route_obs_len, sizeof(char));

		for (long long c = 0; c < channels; c++) {
			sprintf(route_obs, route_obs_format, c);
			obs_property_list_add_int(list, route_obs, c);
		}

		//don't memory leak
		free(route_obs);

		return true;
	}

	/*****************************************************************************/
	static obs_properties_t *rematrix_properties(void *data)
	{
		UNUSED_PARAMETER(data);

		obs_properties_t *props = obs_properties_create();

		//make a list long enough for the maximum # of chs
		obs_property_t *route[MAX_AV_PLANES];
		//pseduo-pan w/ gain (thanks Matt)
		obs_property_t *gain[MAX_AV_PLANES];

		size_t channels = audio_output_get_channels(obs_get_audio());

		//make enough space for c strings
		int pad_digits = (int)floor(log10(abs(MAX_AV_PLANES))) + 1;

		//template out the route format
		const char* route_name_format = "route %i";
		size_t route_len = strlen(route_name_format) + pad_digits;
		char* route_name = (char *)calloc(route_len, sizeof(char));

		//template out the format for the json
		const char* route_obs_format = "out.ch.%i";
		size_t route_obs_len = strlen(route_obs_format) + pad_digits;
		char* route_obs = (char *)calloc(route_obs_len, sizeof(char));

		//template out the gain format
		const char* gain_name_format = "gain %i";
		size_t gain_len = strlen(gain_name_format) + pad_digits;
		char* gain_name = (char *)calloc(gain_len, sizeof(char));

		//add an appropriate # of options to mix from
		for (size_t i = 0; i < channels; i++) {
			sprintf(route_name, route_name_format, i);
			sprintf(gain_name, gain_name_format, i);

			sprintf(route_obs, route_obs_format, i);
			route[i] = obs_properties_add_list(props, route_name,
											   route_obs, OBS_COMBO_TYPE_LIST,
											   OBS_COMBO_FORMAT_INT);

			obs_property_set_long_description(route[i],
											  "tooltip");

			obs_property_set_modified_callback(route[i],
											   fill_out_channels);

			gain[i] = obs_properties_add_float_slider(props, gain_name,
													  "Gain.GainDB", -30.0, 30.0, 0.1);
		}

		//don't memory leak
		free(gain_name);
		free(route_name);
		free(route_obs);

		return props;
	}
}

bool RematrixDialog::registerSource()
{
#ifdef _WIN32
        struct obs_source_info rematrixer_filter = {0};
        rematrixer_filter.id = SOURCE_ID;
        rematrixer_filter.type = OBS_SOURCE_TYPE_FILTER;
        rematrixer_filter.output_flags = OBS_SOURCE_AUDIO;
        rematrixer_filter.get_name = rematrix_name;
        rematrixer_filter.create = rematrix_create;
        rematrixer_filter.destroy = rematrix_destroy;
        rematrixer_filter.get_defaults = rematrix_defaults;
        rematrixer_filter.get_properties = rematrix_properties;
        rematrixer_filter.update = rematrix_update;
        rematrixer_filter.filter_audio = rematrix_filter_audio;
#else
	struct obs_source_info rematrixer_filter = {
			.id = SOURCE_ID,
			.type = OBS_SOURCE_TYPE_FILTER,
			.output_flags = OBS_SOURCE_AUDIO,
			.get_name = rematrix_name,
			.create = rematrix_create,
			.destroy = rematrix_destroy,
			.update = rematrix_update,
			.filter_audio = rematrix_filter_audio,
			.get_defaults = rematrix_defaults,
			.get_properties = rematrix_properties,
	};
#endif

	obs_register_source(&rematrixer_filter);
	return true;
}

RematrixDialog::RematrixDialog(QWidget *parent, OBSSource source, OBSBasic *main)
	: QDialog(parent),
	  m_ui(new Ui::Rematrix),
	  m_source(source),
	  m_main(main)
{
	m_ui->setupUi(this);

	setStyleSheet(COMBOBOX_STYLESHEET);

	m_filter = obs_source_get_filter_by_name(m_source, SOURCE_NAME);
	if (m_filter == nullptr) {
		m_filter = obs_source_create(SOURCE_ID, SOURCE_NAME, nullptr, nullptr);
		if (m_filter) {
			const char *sourceName = obs_source_get_name(source);

			blog(LOG_INFO,
				 "User added filter '%s' (%s) "
				 "to source '%s'",
				 SOURCE_NAME, SOURCE_ID, sourceName);

			obs_source_filter_add(source, m_filter);
		}
	}

	obs_data_t *settings = obs_source_get_settings(m_filter);
	rematrix_data *rematrix = (rematrix_data *)rematrix_create(settings, m_filter);

	long long channelsCount = get_obs_output_channels();
	for(int i = 0; i < channelsCount; ++i) {
		QLabel *channelTitle = new QLabel(QString("Output Channel %1").arg(i+1));
		channelTitle->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		m_ui->gridLayout->addWidget(channelTitle, i*2, 0);

		QComboBox *inputChannelComboBox = new QComboBox();
		inputChannelComboBox->setEditable(false);
		fillComboBoxWithValues(inputChannelComboBox);
		int idx = inputChannelComboBox->findData((int)rematrix->route[i].load());
		if (idx != -1) {
			inputChannelComboBox->setCurrentIndex(idx);
		} else {
			inputChannelComboBox->setCurrentIndex(i + 1);
		}
		m_comboBoxes.append(inputChannelComboBox);

		m_ui->gridLayout->addWidget(inputChannelComboBox, i*2, 1);

		QLabel *gainLabel = new QLabel(QString("Gain (db)"));
		gainLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		m_ui->gridLayout->addWidget(gainLabel, i*2 + 1, 0);

		QHBoxLayout *hLayout = new QHBoxLayout();
		QSlider *slider = new QSlider();
		slider->setOrientation(Qt::Horizontal);
		slider->setMinimum(DB_MIN_VALUE * SLIDER_SCALE);
		slider->setMaximum(DB_MAX_VALUE * SLIDER_SCALE);
		slider->setValue(mul_to_db(rematrix->gain[i]) * SLIDER_SCALE);
		QDoubleSpinBox *spinBox = new QDoubleSpinBox();
		spinBox->setMinimum(DB_MIN_VALUE);
		spinBox->setMaximum(DB_MAX_VALUE);
		spinBox->setValue(mul_to_db(rematrix->gain[i]));
		hLayout->addWidget(slider);
		hLayout->addWidget(spinBox);
		m_ui->gridLayout->addLayout(hLayout, i*2 + 1, 1);

		m_spinBoxes.append(spinBox);

		connect(slider, &QSlider::valueChanged, [spinBox, this](int value) {
			if (!this->m_spinBoxClicked) {
				this->m_sliderMoved = true;
				spinBox->setValue(value / SLIDER_SCALE);
				this->m_sliderMoved = false;
			}
		});

		connect(spinBox, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged), [slider, this](double value) {
			if (!this->m_sliderMoved) {
				this->m_spinBoxClicked = true;
				slider->setValue(value * SLIDER_SCALE);
				this->m_spinBoxClicked = false;
			}
		});
	}

	connect(m_ui->buttonBox, &QDialogButtonBox::clicked, this, &RematrixDialog::on_buttonBox_clicked);
}

RematrixDialog::~RematrixDialog()
{
}

void RematrixDialog::fillComboBoxWithValues(QComboBox* control)
{
	control->addItem("Mute", -1);

	long long channelsCount = get_obs_output_channels();
	for(int i = 0; i < channelsCount; ++i) {
		control->addItem(QString("Input Channel %1").arg(i), i);
	}
}

void RematrixDialog::closeEvent(QCloseEvent *event)
{
	QDialog::closeEvent(event);
}

void RematrixDialog::reject()
{
	QDialog::reject();
}

void RematrixDialog::on_buttonBox_clicked(QAbstractButton *button)
{
	QDialogButtonBox::ButtonRole val = m_ui->buttonBox->buttonRole(button);
	if (val == QDialogButtonBox::AcceptRole) {

		obs_data_t *settings = obs_source_get_settings(m_filter);

		int pad_digits = (int)floor(log10(abs(MAX_AV_PLANES))) + 1;

		const char* route_name_format = "route %i";
		size_t route_len = strlen(route_name_format) + pad_digits;
		char* route_name = (char *)calloc(route_len, sizeof(char));

		const char* gain_name_format = "gain %i";
		size_t gain_len = strlen(gain_name_format) + pad_digits;
		char* gain_name = (char *)calloc(gain_len, sizeof(char));

		long long channelsCount = get_obs_output_channels();
		for(int i = 0; i < channelsCount; ++i) {
			sprintf(route_name, route_name_format, i);
			sprintf(gain_name, gain_name_format, i);


			obs_data_set_int(settings, route_name, m_comboBoxes[i]->currentData().toLongLong());
			obs_data_set_double(settings, gain_name, m_spinBoxes[i]->value());
		}

		obs_source_update(m_filter, settings);
		obs_source_save(m_filter);
		obs_source_save(m_source);

		m_main->SaveProject();
	}
	close();
}
