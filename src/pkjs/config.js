// Settings page, rendered on the phone by Clay (@rebble/clay).
module.exports = [
  {
    type: 'heading',
    defaultValue: 'Poster'
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Status strip' },
      { type: 'text', defaultValue: 'Choose what shows along the bottom of the screen.' },
      { type: 'toggle', messageKey: 'SHOW_WEATHER', label: 'Weather', defaultValue: true },
      { type: 'toggle', messageKey: 'SHOW_HEART_RATE', label: 'Heart rate', defaultValue: true },
      { type: 'toggle', messageKey: 'SHOW_STEPS', label: 'Steps', defaultValue: true },
      { type: 'toggle', messageKey: 'SHOW_BATTERY', label: 'Battery', defaultValue: true }
    ]
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Time' },
      { type: 'toggle', messageKey: 'OVERLAP_DIGITS', label: 'Overlap hours and minutes', defaultValue: false },
      { type: 'text', defaultValue: 'Interlocks the two rows, with the hours drawn over the minutes.' }
    ]
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Weather' },
      {
        type: 'radiogroup',
        messageKey: 'TEMP_UNIT',
        label: 'Temperature unit',
        defaultValue: 'C',
        options: [
          { label: 'Celsius (°C)', value: 'C' },
          { label: 'Fahrenheit (°F)', value: 'F' }
        ]
      }
    ]
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Date' },
      {
        type: 'select',
        messageKey: 'DATE_FORMAT',
        label: 'Vertical date format',
        defaultValue: '0',
        options: [
          { label: '25.11.2026 (DE / EU)', value: '0' },
          { label: '11/25/2026 (US)', value: '1' },
          { label: '2026-11-25 (ISO)', value: '2' },
          { label: '25 NOV 2026', value: '3' },
          { label: 'NOV 25 2026', value: '4' }
        ]
      }
    ]
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Hourly vibration' },
      {
        type: 'select',
        messageKey: 'HOURLY_VIBE',
        label: 'Pattern',
        defaultValue: '0',
        options: [
          { label: 'Off', value: '0' },
          { label: 'Single short', value: '1' },
          { label: 'Double tap', value: '2' },
          { label: 'Long', value: '3' },
          { label: 'Heartbeat', value: '4' }
        ]
      },
      { type: 'text', defaultValue: 'Stays silent during Quiet Time.' }
    ]
  },
  {
    type: 'submit',
    defaultValue: 'Save'
  }
];
