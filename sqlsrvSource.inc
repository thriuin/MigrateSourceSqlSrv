<?php

/**
 * @file
 * Define a MigrateSource for importing from Microsoft SQL Server databases.
 */

/**
 * Implementation of MigrateSource, to handle imports from remote Microsoft SQL Server 
 * servers. This MigrateSource uses the Sql Server extenstion for PHP 5 from Microsoft and
 * is compatible with PHP 5.2 and above.
 */
class MigrateSourceSQLServer extends MigrateSource {
  
  /**
   * The MS SQL Server database instance
   * 
   * @var string 
   */
  protected $host;
  
  /**
   * Array containing information for connecting to SQL Server:
   * - server: The SQL Server instance to connect to
   * - UID: Username to connect as
   * - PWD: Password for logging in
   * - database: (optional) Database to select after connecting
   *
   * @var array
   */
  protected $configuration;

  /**
   * The active MS SQL Server connection for this source.
   *
   * @var resource
   */
  protected $connection;

  /**
   * The SQL query from which to obtain data. Is a string.
   * 
   * @var string
   */
  protected $query;

  /**
   * The result object from executing the query - traversed to process the
   * incoming data.
   * 
   * @var resource
   */
  protected $result;

  /**
   * Number of eligible rows processed so far (used for item limit checking)
   *
   * @var int
   */
  protected $numProcessed = 0;

  /**
   * By default, mssql_query fetches all results - severe memory problems with
   * big tables. So, we will fetch a batch at a time.
   *
   * @var int
   */
  protected $batchSize;

  /**
   * Return an options array for MS SQL sources.
   *
   * @param int $batch_size
   *  Number of rows to pull at once (defaults to 500).
   * @param boolean $cache_counts
   *  Indicates whether to cache counts of source records.
   */
  static public function options($batch_size, $cache_counts) {
    return compact('batch_size', 'cache_counts');
  }

  /**
   * Constructs a MigrateSourceSqlSrv object.
   * 
   * @param string $serverName
   *   Instance, or host, name for the MS SQl Server database
   * @param array $configuration
   *   SQL Server connection configuration information
   * @param string $query
   *   T-SQL select statement to query that migration data
   * @param int $count_query
   *   T-SQL select statement to get a count of the number of rows the data 
   *   query will return.
   * @param array $fields
   *   An array of strings containing the names of the fields that will be
   *   returned.
   * @param array $options
   *   An array of batch options
   *   - batch_size: number of rows to return at a time. Default value if 500.
   */
  public function __construct($serverName, array $configuration, $query, $count_query,
      array $fields, array $options = array()) {
    parent::__construct($options);
    $this->host = $serverName;
    $this->query = $query;
    $this->countQuery = $count_query;
    $this->configuration = $configuration;
    $this->fields = $fields;
    $this->batchSize = isset($options['batch_size']) ? $options['batch_size'] : 500;
  }

  /**
   * Return a string representing the source query.
   *
   * @return string
   */
  public function __toString() {
    return $this->query;
  }

  /**
   * Connect lazily to the DB server.
   * 
   * @return boolean
   *   Returns TRUE if the connection was made, otherwise returns FALSE.
   */
  protected function connect() {
    if (!isset($this->connection)) {
      if (!extension_loaded('sqlsrv')) {
        throw new Exception(t('You must configure the sqlsrv extension in PHP.'));
      }

      $host = $this->host;

      $this->connection = sqlsrv_connect(
        $host,
        $this->configuration);
      if (isset($this->connection)) {
        return TRUE;
      }
      else {
        return FALSE;
      }
    }
  }

  /**
   * Returns a list of fields available to be mapped from the source query.
   *
   * @return array
   *  Keys: machine names of the fields (to be passed to addFieldMapping)
   *  Values: Human-friendly descriptions of the fields.
   */
  public function fields() {
    // The fields are passed to the constructor for this plugin.
    return $this->fields;
  }

  /**
   * Return a count of all available source records.
   * 
   * @return int
   *   The number of available source records.
   */
  public function computeCount() {
    migrate_instrument_start('MigrateSourceSQLServer count');
    if ($this->connect()) {
      $result = sqlsrv_query($this->connection, $this->countQuery);
      $sqlRow = sqlsrv_fetch_array($result);
      $count = reset($sqlRow);
      sqlsrv_free_stmt($result);
      sqlsrv_close($this->connection);
    }
    else {
      // Do something else?
      $count = FALSE;
    }
    migrate_instrument_stop('MigrateSourceSQLServer count');
    return $count;
  }

  /**
   * Implements Iterator::rewind().
   * 
   * This method is called before beginning a foreach loop by the Migrate Module.
   */
  public function rewind() {
    $migration = Migration::currentMigration();
    $this->result = NULL;
    $this->currentRow = NULL;
    $this->numProcessed = 0;
    $this->numIgnored = 0;
    $map = $migration->getMap();

    $keys = array();
    foreach ($map->getSourceKey() as $field_name => $field_schema) {
      // Allow caller to provide an alias to table containing the primary key.
      if (!empty($field_schema['alias'])) {
        $field_name = $field_schema['alias'] . '.' . $field_name;
      }
      $keys[] = $field_name;
    }

     // Replace :criteria placeholder with idlist or highwater clauses. We
     // considered supporting both but it is not worth the complexity. Run twice
     // instead.
     
    $idlist = $migration->getOption('idlist');
    if (isset($idlist)) {
      // TODO: Sanitize. not critical as this is admin supplied data in drush.
      $this->query = str_replace(':criteria',
        $keys[0] . ' IN (' . $idlist . ')', $this->query);
    }
    else {
      $highwaterField = $migration->getHighwaterField();
      if (isset($highwaterField['name']) && $highwater = $migration->getHighwater()) {
        if (empty($highwaterField['alias'])) {
          $highwater_name = $highwaterField['name'];
        }
        else {
          $highwater_name = $highwaterField['alias'] . '.' . $highwaterField['name'];
        }
        $this->query = str_replace(':criteria', "$highwater_name > '$highwater'", $this->query);
      }
      else {
        // No idlist or highwater. Replace :criteria placeholder with harmless WHERE
        // clause instead of empty since we don't know if an AND follows.
        $this->query = str_replace(':criteria', '1=1', $this->query);
      }
    }

    migrate_instrument_start('sqlsrv_query');

    if (!isset($this->connection)) {
      $this->connect();
    }
    $this->result = sqlsrv_query($this->connection, $this->query);
    migrate_instrument_stop('sqlsrv_query');

    $this->next();
  }

  /**
   * Implementation of Iterator::next().
   * 
   * This method is called at the bottom of the loop implicitly,
   * as well as explicitly from rewind().
   */
  public function next() {
    migrate_instrument_start('MigrateSourceSQLServer next');

    $migration = Migration::currentMigration();

    // Enforce the itemlimit
    $itemlimit = $migration->getItemLimit();
    while ($this->currentRow = $this->getNextObject()) {
      // Enforce the itemlimit
      if ($itemlimit && $this->numProcessed >= $itemlimit) {
        $this->currentRow = NULL;
        migrate_instrument_stop('MigrateSourceMSSQL next');
        return;
      }

      // Check the map - if it's already mapped, and not marked for update, skip it
      $map = $migration->getMap();
      $this->currentKey = array();
      foreach ($map->getSourceKey() as $field_name => $field_schema) {
        $this->currentKey[$field_name] = $this->currentRow->$field_name;
      }
      $map_row = $map->getRowBySource($this->currentKey);
      if ($map_row && $map_row['needs_update'] == 0) {
        continue;
      }

      // Allow the Migration to prepare this row. prepareRow() can return boolean
      // FALSE to stop processing this row. To add/modify fields on the
      // result, modify $row by reference.
      $return = TRUE;
      if (method_exists($migration, 'prepareRow')) {
        $return = $migration->prepareRow($this->currentRow);
      }

      if ($return !== FALSE) {
        $this->numProcessed++;
        break;
      }
      else {
        $this->numIgnored++;
      }
    }
    if (!is_object($this->currentRow)) {
      $this->currentRow = NULL;
    }
    migrate_instrument_stop('MigrateSourceSQLServer next');
  }

  /**
   * Convenience function to return the next row of the result set as an
   * object, dealing with the difference between the end of the batch and the
   * end of all data.
   * 
   * @return boolean
   * Returns TRUE if result was retrieved, otherwise returns FALSE.
   */
  public function getNextObject() {
    migrate_instrument_start('sqlsrv_fetch_object');
    $row = sqlsrv_fetch_object($this->result);
    migrate_instrument_stop('sqlsrv_fetch_object');

    if (is_object($row)) {
      return $row;
    }
    else {
      return FALSE;
    }
  }
}
