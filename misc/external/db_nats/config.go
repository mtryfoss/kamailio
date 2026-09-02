package main

import (
	"fmt"
	"os"
	"time"

	"gopkg.in/yaml.v3"
)

type Config struct {
	NATS struct {
		URL           string `yaml:"url"`
		QueueGroup    string `yaml:"queue_group"`
		SubjectPrefix string `yaml:"subject_prefix"`
		CredsFile     string `yaml:"creds_file"`
		// Raw SQL passthrough is off unless explicitly enabled. Prefer also
		// denying the .raw subject in the NATS account permissions, so the
		// bus enforces it rather than this process.
		EnableRaw bool `yaml:"enable_raw"`
	} `yaml:"nats"`

	MySQL struct {
		DSN          string `yaml:"dsn"`
		MaxOpenConns int    `yaml:"max_open_conns"`
		MaxIdleConns int    `yaml:"max_idle_conns"`
		// Timezone used to convert between MySQL DATETIME and the epoch
		// seconds carried on the wire. "Local" matches db_mysql's behaviour.
		Timezone string `yaml:"timezone"`
		// Report UNSIGNED columns as DB1_UINT/DB1_UBIGINT. Off by default,
		// matching db_mysql's unsigned_type modparam.
		UnsignedType bool `yaml:"unsigned_type"`
	} `yaml:"mysql"`

	Cursors struct {
		IdleTTL time.Duration `yaml:"idle_ttl"`
		Max     int           `yaml:"max"`
		MaxRows int           `yaml:"max_rows_per_page"`
	} `yaml:"cursors"`

	Shutdown struct {
		// How long to keep serving open cursors after a signal, before
		// closing them anyway. Clients paging through a large table would
		// otherwise lose the cursor mid-read on every restart.
		Grace time.Duration `yaml:"grace"`
	} `yaml:"shutdown"`

	Grants []TableGrant `yaml:"grants"`

	loc *time.Location
}

func LoadConfig(path string) (*Config, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}

	var c Config
	if err := yaml.Unmarshal(b, &c); err != nil {
		return nil, fmt.Errorf("parsing %s: %w", path, err)
	}

	if c.NATS.URL == "" {
		c.NATS.URL = "nats://127.0.0.1:4222"
	}
	if c.NATS.QueueGroup == "" {
		c.NATS.QueueGroup = "kamailio-db"
	}
	if c.NATS.SubjectPrefix == "" {
		c.NATS.SubjectPrefix = "kamailio.db"
	}
	if c.MySQL.DSN == "" {
		return nil, fmt.Errorf("mysql.dsn is required")
	}
	if c.MySQL.MaxOpenConns == 0 {
		c.MySQL.MaxOpenConns = 16
	}
	if c.MySQL.MaxIdleConns == 0 {
		c.MySQL.MaxIdleConns = 8
	}
	if c.Cursors.IdleTTL == 0 {
		c.Cursors.IdleTTL = 60 * time.Second
	}
	if c.Cursors.Max == 0 {
		c.Cursors.Max = 64
	}
	if c.Cursors.MaxRows == 0 {
		c.Cursors.MaxRows = 10000
	}
	if c.Shutdown.Grace == 0 {
		c.Shutdown.Grace = 30 * time.Second
	}

	if c.MySQL.Timezone == "" {
		c.MySQL.Timezone = "Local"
	}
	c.loc, err = time.LoadLocation(c.MySQL.Timezone)
	if err != nil {
		return nil, fmt.Errorf("bad mysql.timezone %q: %w", c.MySQL.Timezone, err)
	}

	if len(c.Grants) == 0 {
		return nil, fmt.Errorf("no grants configured: the backend would deny every request")
	}
	for i := range c.Grants {
		if err := c.Grants[i].normalize(); err != nil {
			return nil, err
		}
	}

	return &c, nil
}
